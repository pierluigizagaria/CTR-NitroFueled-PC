/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Native adapter for the versioned loopback relay. Steam and PSN semantic
 * mapping belong to the bridge's provider; guest callbacks belong to psn-host.
 */
#include "../include/psn_backend.h"
#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <curl/curl.h>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
constexpr size_t body_limit = 256 * 1024, wire_limit = 1024 * 1024;
constexpr size_t event_limit = 1024, job_limit = 128;
struct Invalid : std::runtime_error {
    using std::runtime_error::runtime_error;
};
void require(bool b) {
    if (!b)
        throw Invalid("invalid bridge contract");
}
std::string text(const Json &j, size_t max, bool empty = true) {
    require(j.is_string());
    auto s = j.get<std::string>();
    require(s.size() <= max && (empty || !s.empty()) && s.find('\0') == std::string::npos);
    return s;
}
std::string input(const char *s, size_t max, bool empty = true) {
    require(s != nullptr);
    size_t n = 0;
    while (n <= max && s[n])
        ++n;
    require(n <= max && (empty || n));
    return std::string(s, n);
}
uint64_t decimal(const Json &j, bool zero = false) {
    auto s = text(j, 20, false);
    require(s.size() == 1 || s[0] != '0');
    uint64_t n = 0;
    auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), n);
    require(ec == std::errc{} && p == s.data() + s.size() && (zero || n));
    return n;
}
uint64_t number(const Json &j) {
    require(j.is_number_unsigned() || (j.is_number_integer() && j.get<int64_t>() >= 0));
    return j.get<uint64_t>();
}
int32_t integer(const Json &j) {
    require(j.is_number_integer());
    auto n = j.get<int64_t>();
    require(n >= INT32_MIN && n <= INT32_MAX);
    return static_cast<int32_t>(n);
}
template <size_t N> void copy(char (&out)[N], const std::string &s) {
    require(s.size() < N);
    std::memset(out, 0, N);
    std::memcpy(out, s.data(), s.size());
}
std::string hex(PsnBytes bytes) {
    require(bytes.size <= body_limit && (!bytes.size || bytes.data));
    constexpr char digits[] = "0123456789abcdef";
    std::string out(static_cast<size_t>(bytes.size) * 2, '0');
    auto p = static_cast<const unsigned char *>(bytes.data);
    for (size_t i = 0; i < bytes.size; ++i) {
        out[2 * i] = digits[p[i] >> 4];
        out[2 * i + 1] = digits[p[i] & 15];
    }
    return out;
}
std::string unhex(const Json &j) {
    auto s = text(j, body_limit * 2);
    require(!(s.size() & 1));
    auto nibble = [](char c) {
        require((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
        return c <= '9' ? c - '0' : c - 'a' + 10;
    };
    std::string out(s.size() / 2, '\0');
    for (size_t i = 0; i < out.size(); ++i)
        out[i] = static_cast<char>((nibble(s[2 * i]) << 4) | nibble(s[2 * i + 1]));
    return out;
}
int32_t status(int32_t s) {
    switch (s) {
    case 0:
        return 0;
    case -1:
        return PSN_INVALID_ARGUMENT;
    case -2:
        return PSN_SIGNED_OUT;
    case -4:
        return PSN_BACKEND_UNSUPPORTED;
    case -5:
        return PSN_BACKEND_ABORTED;
    case -7:
        return PSN_BACKEND_BUSY;
    default:
        return PSN_BACKEND_UNAVAILABLE;
    }
}
struct Job {
    enum Kind { web, auth, invitation } kind = web;
    bool active = false, done = false;
    std::atomic<bool> canceled{false};
    Json request;
    PsnAuthResult auth_result{};
    PsnInvitationUser recipient{};
    PsnInvitationResult invitation_result{};
    std::string session;
    int32_t result = 0, http = 0;
    std::string body, content_type;
    PsnHeader header{};
    Clock::time_point queued_at{}, started_at{}, completed_at{};
    const char *timing_class = "other";
    bool timing_logged = false;
};
struct Subscription {
    uint32_t kind, label;
    bool service_present;
    std::string service, type;
};
struct Event {
    PsnEvent value{};
    Clock::time_point received_at{};
    bool service_present = false;
    std::string service, type, data;
    std::vector<std::pair<std::string, std::string>> values;
    std::vector<PsnExtData> ext;
    void bind() {
        if (value.kind != 2)
            return;
        auto &p = value.data.push;
        p.service = service_present ? service.c_str() : nullptr;
        p.data_type = type.c_str();
        p.data = {data.data(), data.size()};
        ext.clear();
        ext.reserve(values.size());
        for (auto &v : values)
            ext.push_back({v.first.c_str(), {v.second.data(), v.second.size()}});
        p.ext_data = ext.data();
        p.ext_data_count = static_cast<uint32_t>(ext.size());
    }
};
struct Instance {
    std::string url, token;
    int32_t local_user_id = 1;
    std::atomic<bool> stopping{false};
    std::mutex mutex;
    std::condition_variable wake;
    std::thread worker, event_worker;
    PsnIdentity identity{};
    uint64_t next_job = 1, cursor = 0;
    int32_t failure = 0;
    std::map<uint64_t, std::shared_ptr<Job>> jobs;
    std::deque<uint64_t> pending;
    std::map<uint64_t, Subscription> subscriptions;
    std::deque<std::shared_ptr<Event>> events;
    std::shared_ptr<Event> borrowed;
    bool trace_timing = false;
    Clock::time_point timing_origin = Clock::now();
};
long long milliseconds(Clock::time_point from, Clock::time_point to) noexcept {
    return std::max<long long>(0, std::chrono::duration_cast<std::chrono::milliseconds>(to - from).count());
}
// Only fixed categories reach the timing log. Never emit any caller-supplied
// path, query, account/session identifier, header, name or payload.
const char *web_timing_class(std::string_view path, uint32_t method) noexcept {
    path = path.substr(0, path.find('?'));
    if (path == "/v1/sessions" && method == 1)
        return "session_create";
    constexpr std::string_view prefix = "/v1/sessions/";
    if (!path.starts_with(prefix))
        return "other";
    path.remove_prefix(prefix.size());
    auto end = path.find('/');
    auto session = path.substr(0, end);
    if (session.empty() || session.find_first_not_of("0123456789") != session.npos)
        return "other";
    auto suffix = end == path.npos ? std::string_view{} : path.substr(end);
    if (suffix.empty())
        return method == 0 ? "session_read" : method == 3 ? "session_delete" : "session_update";
    if (suffix == "/members")
        return method == 1 ? "membership_add" : "members";
    if (suffix.starts_with("/members/") && method == 3)
        return "membership_leave";
    if (suffix == "/sessionData")
        return "session_data";
    if (suffix == "/changeableSessionData")
        return "session_changeable_data";
    if (suffix == "/sessionImage")
        return "session_image";
    if (suffix == "/invitations")
        return "session_invitations";
    return "other";
}
void trace_web_completion(Instance &i, Job &job) noexcept {
    if (!i.trace_timing || job.timing_logged)
        return;
    job.timing_logged = true;
    const auto now = Clock::now();
    const auto started = job.started_at == Clock::time_point{} ? job.completed_at : job.started_at;
    std::fprintf(stderr,
        "PSN timing: web_complete t_ms=%lld class=%s queue_ms=%lld service_ms=%lld consume_ms=%lld result=%d http=%d\n",
        milliseconds(i.timing_origin, now), job.timing_class,
        milliseconds(job.queued_at, started), milliseconds(started, job.completed_at),
        milliseconds(job.completed_at, now), job.result, job.http);
}
struct Transfer {
    Instance *instance;
    Job *job;
    std::string body;
    bool overflow = false;
};
size_t receive(char *ptr, size_t size, size_t count, void *opaque) {
    auto &t = *static_cast<Transfer *>(opaque);
    if (count && size > wire_limit / count) {
        t.overflow = true;
        return 0;
    }
    size_t n = size * count;
    if (n > wire_limit - t.body.size()) {
        t.overflow = true;
        return 0;
    }
    try {
        t.body.append(ptr, n);
    } catch (...) {
        return 0;
    }
    return n;
}
int progress(void *opaque, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    auto &t = *static_cast<Transfer *>(opaque);
    return t.instance->stopping || (t.job && t.job->canceled);
}
struct Reply {
    CURLcode transport;
    long http;
    Json json;
};
Reply http(Instance &i, const std::string &path, const Json *body = nullptr, Job *job = nullptr) {
    if (i.stopping || (job && job->canceled))
        return {CURLE_ABORTED_BY_CALLBACK, 0, {}};
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
    require(curl != nullptr);
    Transfer transfer{&i, job, {}, false};
    auto url = i.url + path;
    auto payload = body ? body->dump() : std::string{};
    require(payload.size() <= wire_limit);
    auto authorization = "Authorization: Bearer " + i.token;
    curl_slist *raw = nullptr;
    raw = curl_slist_append(raw, authorization.c_str());
    raw = curl_slist_append(raw, "Content-Type: application/json");
    std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers(raw, curl_slist_free_all);
    require(raw != nullptr);
    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, raw);
    curl_easy_setopt(curl.get(), CURLOPT_PROXY, "");
    curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl.get(), CURLOPT_PROTOCOLS_STR, "http");
    curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT_MS, 1000L);
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT_MS, job ? 30000L : 2000L);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, receive);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &transfer);
    curl_easy_setopt(curl.get(), CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl.get(), CURLOPT_XFERINFOFUNCTION, progress);
    curl_easy_setopt(curl.get(), CURLOPT_XFERINFODATA, &transfer);
    if (body) {
        curl_easy_setopt(curl.get(), CURLOPT_POST, 1L);
        curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, payload.data());
        curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE_LARGE,
                         static_cast<curl_off_t>(payload.size()));
    }
    auto result = curl_easy_perform(curl.get());
    long code = 0;
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &code);
    require(!transfer.overflow);
    Json parsed;
    if (result == CURLE_OK && code == 200) {
        parsed = Json::parse(transfer.body);
        require(parsed.is_object());
    }
    return {result, code, std::move(parsed)};
}
Json invitation_web(const char *method, const char *group, const std::string &path,
                    const std::string &body = {}) {
    return {{"method", method}, {"api_group", group}, {"path", path},
            {"content_type", "application/json"}, {"headers", Json::object()},
            {"bodyHex", hex({body.data(), body.size()})}};
}
void send_invitation(Instance &i, Job &job, Job &result) {
    // The guest already selected one recipient. Resolve its copied identity
    // through the provider's ordinary friend snapshot; never open another picker.
    auto lookup = invitation_web("GET", "friendList", "/v1/users/me/friendList");
    auto reply = http(i, "/psn/v1/webapi", &lookup, &job);
    if (reply.transport != CURLE_OK || reply.http != 200) {
        result.result = PSN_BACKEND_UNAVAILABLE;
        return;
    }
    result.result = status(integer(reply.json.at("status")));
    if (result.result)
        return;
    if (integer(reply.json.at("httpStatus")) != 200) {
        result.result = PSN_BACKEND_UNAVAILABLE;
        return;
    }
    auto snapshot = Json::parse(unhex(reply.json.at("bodyHex")));
    const auto &friends = snapshot.at("friendList");
    require(friends.is_array() && friends.size() <= 5000 &&
            number(snapshot.at("start")) == 0 && number(snapshot.at("size")) == friends.size() &&
            number(snapshot.at("totalResults")) == friends.size());
    std::set<uint64_t> accounts;
    unsigned matches = 0;
    PsnInvitationUser selected{};
    for (const auto &entry : friends) {
        PsnInvitationUser candidate{};
        const auto &user = entry.at("user");
        candidate.account_id = decimal(user.at("accountId"));
        copy(candidate.online_id, text(user.at("onlineId"), 16, false));
        require(accounts.insert(candidate.account_id).second);
        if (entry.at("presence").at("onlineStatus") != "online")
            continue;
        if (job.recipient.account_id ? candidate.account_id == job.recipient.account_id
                                     : !std::strcmp(candidate.online_id, job.recipient.online_id)) {
            selected = candidate;
            ++matches;
        }
    }
    if (matches != 1) {
        result.result = PSN_BACKEND_UNAVAILABLE;
        return;
    }
    // Cancel between the read and mutation must not submit an invitation.
    // Cancellation after dispatch cannot retract a platform invitation.
    if (i.stopping || job.canceled) {
        result.result = PSN_BACKEND_ABORTED;
        return;
    }
    const auto body = Json{{"to", {std::to_string(selected.account_id)}}}.dump();
    auto send = invitation_web("POST", "sessionInvitation",
                               "/v1/sessions/" + job.session + "/invitations", body);
    reply = http(i, "/psn/v1/webapi", &send, &job);
    // Mutations are never retried after an uncertain transport outcome.
    if (reply.transport != CURLE_OK || reply.http != 200) {
        result.result = PSN_BACKEND_UNAVAILABLE;
        return;
    }
    result.result = status(integer(reply.json.at("status")));
    if (result.result)
        return;
    if (integer(reply.json.at("httpStatus")) != 204) {
        result.result = PSN_BACKEND_UNAVAILABLE;
        return;
    }
    result.invitation_result.user_count = 1;
    result.invitation_result.users[0] = selected;
}
void fail(Instance &i, int32_t error) {
    if (!i.failure)
        i.failure = error;
    i.events.clear();
    for (auto &[id, job] : i.jobs) {
        (void)id;
        job->canceled = true;
        if (!job->done) {
            job->done = true;
            if (i.trace_timing)
                job->completed_at = Clock::now();
            job->result = error;
            job->auth_result.result = error;
        }
    }
    i.wake.notify_all();
}
void peer(const Json &j, uint64_t &account, char (&name)[17], bool unknown = false) {
    require(j.is_object());
    account = decimal(j.at("accountId"), unknown);
    copy(name, text(j.at("onlineId"), 16, unknown));
    require(account || !name[0]);
}
std::shared_ptr<Event> event(const Json &j, const PsnIdentity &identity, int32_t local_user_id) {
    require(j.is_object() && integer(j.at("version")) == 1);
    auto e = std::make_shared<Event>();
    e->value.user_id = local_user_id;
    auto kind = text(j.at("kind"), 32, false);
    if (kind == "identity") {
        e->value.kind = 1;
        auto &id = e->value.data.identity;
        id.account_id = decimal(j.at("accountId"), true);
        copy(id.online_id, text(j.at("onlineId"), 16));
        require(j.at("signedIn").is_boolean());
        id.state = j.at("signedIn").get<bool>() ? 2 : 1;
        require(id.state == 1 || (id.account_id && id.online_id[0]));
        return e;
    }
    uint64_t from = 0, to = 0;
    char from_name[17]{}, to_name[17]{};
    peer(j.at("from"), from, from_name, kind == "push");
    peer(j.at("to"), to, to_name);
    require(to == identity.account_id);
    if (kind == "invitationAccepted") {
        e->value.kind = 3;
        auto &v = e->value.data.invitation;
        copy(v.session_id, text(j.at("sessionId"), 45, false));
        copy(v.invitation_id, text(j.at("invitationId"), 60, false));
        v.flag = 1;
        v.accepting_user_id = local_user_id;
        v.referring_account_id = from;
        copy(v.accepting_online_id, to_name);
        copy(v.referring_online_id, from_name);
    } else {
        require(kind == "push");
        e->value.kind = 2;
        auto &p = e->value.data.push;
        p.from_account_id = from;
        p.to_account_id = to;
        copy(p.from_online_id, from_name);
        copy(p.to_online_id, to_name);
        p.from_platform = p.to_platform = 3;
        e->service_present = !j.at("service").is_null();
        if (e->service_present)
            e->service = text(j.at("service"), 255);
        auto label = number(j.at("serviceLabel"));
        require(label <= UINT32_MAX);
        p.service_label = static_cast<uint32_t>(label);
        e->type = text(j.at("dataType"), 64, false);
        e->data = text(j.at("data"), 65536);
        const auto &ext = j.at("extData");
        require(ext.is_object() && ext.size() <= PSN_MAX_EXT_DATA);
        for (auto it = ext.begin(); it != ext.end(); ++it) {
            require(!it.key().empty() && it.key().size() <= 32 &&
                    it.key().find('\0') == std::string::npos);
            e->values.emplace_back(it.key(), text(it.value(), 4096));
        }
    }
    return e;
}
void poll_events(Instance &i) {
    uint64_t cursor;
    {
        std::lock_guard lock(i.mutex);
        if (i.failure)
            return;
        cursor = i.cursor;
    }
    const auto fetch_started = i.trace_timing ? Clock::now() : Clock::time_point{};
    auto reply = http(i, "/psn/v1/events?after=" + std::to_string(cursor));
    if (reply.transport != CURLE_OK)
        throw std::runtime_error("transport retry");
    require(reply.http == 200);
    auto &j = reply.json;
    require(integer(j.at("version")) == 1);
    auto &list = j.at("events");
    require(list.is_array() && list.size() <= 64);
    std::lock_guard lock(i.mutex);
    std::deque<std::shared_ptr<Event>> batch;
    for (auto &entry : list) {
        uint64_t sequence = number(entry.at("sequence"));
        if (sequence <= cursor)
            continue; // A retried response never replays a delivered invitation.
        require(sequence == cursor + 1);
        auto e = event(entry.at("event"), i.identity, i.local_user_id);
        if (i.trace_timing && e->value.kind == 3)
            e->received_at = Clock::now();
        if (e->value.kind == 1) {
            auto id = e->value.data.identity;
            require(id.state == 2 && id.account_id == i.identity.account_id);
            i.identity = id;
        }
        if (e->value.kind == 2) {
            for (auto &[cookie, sub] : i.subscriptions) {
                bool match =
                    e->type == sub.type && e->service_present == sub.service_present &&
                    e->service == sub.service &&
                    (!sub.service_present || e->value.data.push.service_label == sub.label);
                if (match) {
                    auto clone = std::make_shared<Event>(*e);
                    clone->value.data.push.subscription = cookie;
                    batch.push_back(std::move(clone));
                }
            }
        } else
            batch.push_back(std::move(e));
        require(batch.size() + i.events.size() <= event_limit);
        cursor = sequence;
    }
    i.events.insert(i.events.end(), batch.begin(), batch.end());
    if (i.trace_timing)
        for (const auto &e : batch)
            if (e->value.kind == 3)
                std::fprintf(stderr,
                    "PSN timing: invitation_received t_ms=%lld fetch_ms=%lld\n",
                    milliseconds(i.timing_origin, e->received_at),
                    milliseconds(fetch_started, e->received_at));
    i.cursor = cursor;
}
void run(Instance &i) noexcept {
    while (!i.stopping) {
        std::shared_ptr<Job> job;
        {
            std::unique_lock lock(i.mutex);
            if (i.failure) {
                i.wake.wait(lock, [&] { return i.stopping.load(); });
                break;
            }
            while (!i.pending.empty()) {
                auto id = i.pending.front();
                i.pending.pop_front();
                auto found = i.jobs.find(id);
                if (found != i.jobs.end() && !found->second->done) {
                    job = found->second;
                    job->active = true;
                    if (i.trace_timing)
                        job->started_at = Clock::now();
                    break;
                }
            }
        }
        if (job) {
            // A private result prevents cancel/poll from racing fields filled by
            // curl.
            Job result;
            try {
                if (job->kind == Job::invitation) {
                    send_invitation(i, *job, result);
                } else {
                    // Use the real job's atomic cancellation during the transfer.
                    auto reply = http(i, job->kind == Job::auth ? "/psn/v1/token" : "/psn/v1/webapi",
                                      &job->request, job.get());
                    if (reply.transport != CURLE_OK || reply.http != 200)
                        result.result = PSN_BACKEND_UNAVAILABLE;
                    else {
                        auto &j = reply.json;
                        result.result = status(integer(j.at("status")));
                        if (job->kind == Job::auth && !result.result) {
                            result.auth_result.issuer_id = integer(j.at("issuerId"));
                            copy(result.auth_result.value, text(j.at("token"), 4096, false));
                        } else if (job->kind == Job::web && !result.result) {
                            result.http = integer(j.at("httpStatus"));
                            require(result.http >= 100 && result.http <= 599);
                            result.content_type = text(j.at("contentType"), 1024, false);
                            result.body = unhex(j.at("bodyHex"));
                        }
                    }
                }
            } catch (...) {
                result.result = PSN_BACKEND_UNAVAILABLE;
            }
            std::lock_guard lock(i.mutex);
            if (!job->done) {
                job->result = result.result;
                job->auth_result = result.auth_result;
                job->auth_result.result = result.result;
                job->invitation_result = result.invitation_result;
                job->http = result.http;
                job->body = std::move(result.body);
                job->content_type = std::move(result.content_type);
                if (i.trace_timing)
                    job->completed_at = Clock::now();
                job->done = true;
            }
            job->active = false;
            i.wake.notify_all();
        }
        std::unique_lock lock(i.mutex);
        i.wake.wait(lock, [&] { return i.stopping || !i.pending.empty() || i.failure; });
    }
}
void run_events(Instance &i) noexcept {
    unsigned failures = 0;
    while (!i.stopping) {
        try {
            poll_events(i);
            failures = 0;
        } catch (const Invalid &) {
            std::lock_guard lock(i.mutex);
            fail(i, PSN_BACKEND_UNAVAILABLE);
        } catch (const Json::exception &) {
            std::lock_guard lock(i.mutex);
            fail(i, PSN_BACKEND_UNAVAILABLE);
        } catch (...) {
            if (++failures >= 5) {
                std::lock_guard lock(i.mutex);
                fail(i, PSN_BACKEND_UNAVAILABLE);
            }
        }
        std::unique_lock lock(i.mutex);
        if (i.failure)
            i.wake.wait(lock, [&] { return i.stopping.load(); });
        else
            i.wake.wait_for(lock, std::chrono::milliseconds(100),
                            [&] { return i.stopping.load(); });
    }
}
template <class F> int32_t guarded(F f) noexcept {
    try {
        return f();
    } catch (const std::bad_alloc &) {
        return PSN_BACKEND_NO_MEMORY;
    } catch (...) {
        return PSN_INVALID_ARGUMENT;
    }
}
int32_t open(const char *configuration, void **out) {
    if (!out)
        return PSN_INVALID_ARGUMENT;
    *out = nullptr;
    return guarded([&]() -> int32_t {
        auto j = Json::parse(input(configuration, 4096, false));
        require(j.is_object() && (j.size() == 2 || (j.size() == 3 && j.contains("local_user_id"))));
        auto i = std::make_unique<Instance>();
        const char *timing = std::getenv("PSN_BRIDGE_TRACE_TIMING");
        i->trace_timing = timing && !std::strcmp(timing, "1");
        if (j.contains("local_user_id")) {
            i->local_user_id = integer(j.at("local_user_id"));
            require(i->local_user_id >= 0);
        }
        i->url = text(j.at("url"), 64, false);
        i->token = text(j.at("token"), 64, false);
        require(i->token.size() == 64 &&
                i->token.find_first_not_of("0123456789abcdef") == std::string::npos);
        // Numeric 127/8 only: no DNS, IPv6 ambiguity, URL credentials, path or
        // proxy.
        require(i->url.starts_with("http://127."));
        auto host = i->url.substr(7);
        auto colon = host.find(':');
        require(colon != std::string::npos && host.find(':', colon + 1) == std::string::npos);
        auto ip = host.substr(0, colon);
        size_t pos = 0;
        for (unsigned part = 0; part < 4; ++part) {
            size_t end = ip.find('.', pos);
            require((part == 3) == (end == std::string::npos));
            auto s = ip.substr(pos, end == std::string::npos ? end : end - pos);
            uint32_t value = 0;
            auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
            require(!s.empty() && s.size() <= 3 && (s.size() == 1 || s[0] != '0') &&
                    ec == std::errc{} && p == s.data() + s.size() && value <= 255);
            pos = end + 1;
        }
        auto port = host.substr(colon + 1);
        uint32_t n = 0;
        auto [p, ec] = std::from_chars(port.data(), port.data() + port.size(), n);
        require(ec == std::errc{} && p == port.data() + port.size() && n && n <= 65535);
        static const int curl_initialized = curl_global_init(CURL_GLOBAL_DEFAULT);
        require(curl_initialized == CURLE_OK);
        auto reply = http(*i, "/psn/v1/info");
        if (reply.transport != CURLE_OK || reply.http != 200) {
            std::fprintf(stderr, "PSN bridge: handshake failed (transport %d, HTTP %ld).\n",
                         static_cast<int>(reply.transport), reply.http);
            return PSN_BACKEND_UNAVAILABLE;
        }
        auto &info = reply.json;
        require(integer(info.at("version")) == 1 && info.at("signedIn") == true);
        i->identity.account_id = decimal(info.at("accountId"));
        copy(i->identity.online_id, text(info.at("onlineId"), 16, false));
        i->identity.state = 2;
        const auto &caps = info.at("capabilities");
        require(caps.is_array());
        for (const char *cap : {"identity", "id-token", "webapi", "push", "invitation-accepted"})
            require(std::find(caps.begin(), caps.end(), cap) != caps.end());
        i->worker = std::thread([ptr = i.get()] { run(*ptr); });
        try {
            i->event_worker = std::thread([ptr = i.get()] { run_events(*ptr); });
        } catch (...) {
            i->stopping = true;
            i->wake.notify_all();
            i->worker.join();
            throw;
        }
        *out = i.release();
        return 0;
    });
}
void close(void *opaque) {
    auto i = static_cast<Instance *>(opaque);
    if (!i)
        return;
    i->stopping = true;
    i->wake.notify_all();
    if (i->worker.joinable())
        i->worker.join();
    if (i->event_worker.joinable())
        i->event_worker.join();
    delete i;
}
int32_t identity(void *opaque, int32_t user, PsnIdentity *out) {
    if (!opaque || !out || user != static_cast<Instance *>(opaque)->local_user_id)
        return PSN_INVALID_ARGUMENT;
    auto &i = *static_cast<Instance *>(opaque);
    std::lock_guard lock(i.mutex);
    if (i.failure)
        return i.failure;
    *out = i.identity;
    return 0;
}
int32_t policy(void *opaque, int32_t user, uint64_t, PsnPolicy *out) {
    PsnIdentity id;
    auto rc = identity(opaque, user, &id);
    if (rc || !out)
        return rc ? rc : PSN_INVALID_ARGUMENT;
    *out = {};
    out->plus_authorized = 1;
    out->age = 99;
    return 0;
}
int32_t title(void *, const char *name, PsnBytes secret) {
    return guarded([&]() -> int32_t {
        auto s = input(name, 12, false);
        require(secret.data && secret.size == 128);
        return 0;
    });
}
int32_t restriction(void *, const PsnContentRestriction *r) {
    if (!r || r->default_age < 0 || r->count > 256 || (r->count && !r->countries))
        return PSN_INVALID_ARGUMENT;
    for (uint32_t n = 0; n < r->count; ++n)
        if (r->countries[n].age < 0 || r->countries[n].country[2])
            return PSN_INVALID_ARGUMENT;
    return 0; // Local adult profile; no claim that Steam enforced a PSN parental
              // policy.
}
int32_t submit(Instance &i, std::shared_ptr<Job> job, uint64_t *out) {
    std::lock_guard lock(i.mutex);
    if (i.failure)
        return i.failure;
    if (i.jobs.size() >= job_limit || i.next_job == UINT64_MAX)
        return PSN_BACKEND_BUSY;
    auto id = i.next_job++;
    if (i.trace_timing)
        job->queued_at = Clock::now();
    i.jobs.emplace(id, std::move(job));
    try {
        i.pending.push_back(id);
    } catch (...) {
        i.jobs.erase(id);
        throw;
    }
    *out = id;
    i.wake.notify_all();
    return 0;
}
int32_t auth_start(void *opaque, const PsnAuthRequest *r, uint64_t *out) {
    if (!opaque || !r || !out || r->user_id != static_cast<Instance *>(opaque)->local_user_id)
        return PSN_INVALID_ARGUMENT;
    if (r->kind != 1)
        return PSN_BACKEND_UNSUPPORTED;
    return guarded([&]() -> int32_t {
        auto job = std::make_shared<Job>();
        job->kind = Job::auth;
        job->request = {{"kind", 1},
                        {"client_id", input(r->client_id, 4096, false)},
                        {"scope", input(r->scope ? r->scope : "", 4096)}};
        // The local capability does not use a Sony application secret. Validate the
        // original call's bounded input without forwarding or logging it.
        if (r->client_secret)
            input(r->client_secret, 4096);
        return submit(*static_cast<Instance *>(opaque), job, out);
    });
}
int32_t auth_poll(void *opaque, uint64_t id, PsnAuthResult *out) {
    if (!opaque || !out)
        return PSN_INVALID_ARGUMENT;
    auto &i = *static_cast<Instance *>(opaque);
    std::lock_guard lock(i.mutex);
    auto found = i.jobs.find(id);
    if (found == i.jobs.end() || found->second->kind != Job::auth)
        return PSN_INVALID_ARGUMENT;
    if (!found->second->done)
        return 1;
    *out = found->second->auth_result;
    return 0;
}
int32_t web_start(void *opaque, const PsnWebRequest *r, uint64_t *out) {
    if (!opaque || !r || !out || r->user_id != static_cast<Instance *>(opaque)->local_user_id ||
        r->method > 4 || r->header_count > PSN_MAX_HEADERS || (r->header_count && !r->headers))
        return PSN_INVALID_ARGUMENT;
    if (r->body.size > body_limit)
        return PSN_BACKEND_UNSUPPORTED;
    return guarded([&]() -> int32_t {
        constexpr const char *methods[] = {"GET", "POST", "PUT", "DELETE", "PATCH"};
        auto path = input(r->path, 8192, false);
        require(path[0] == '/' && !path.starts_with("//") &&
                path.find_first_of("\r\n") == std::string::npos);
        Json headers = Json::object();
        std::set<std::string> names;
        for (uint32_t n = 0; n < r->header_count; ++n) {
            auto name = input(r->headers[n].name, 256, false),
                 value = input(r->headers[n].value, 8192);
            require(name.find_first_of("\r\n:") == std::string::npos &&
                    value.find_first_of("\r\n") == std::string::npos);
            auto lower = name;
            for (auto &c : lower)
                if (c >= 'A' && c <= 'Z')
                    c += 'a' - 'A';
            if (!names.insert(lower).second)
                return PSN_BACKEND_UNSUPPORTED;
            headers[name] = value;
        }
        auto job = std::make_shared<Job>();
        job->timing_class = web_timing_class(path, r->method);
        job->request = {{"method", methods[r->method]},
                        {"api_group", input(r->api_group, 256, false)},
                        {"path", path},
                        {"content_type", input(r->content_type ? r->content_type : "", 1024)},
                        {"headers", headers}};
        if (job->request.dump().size() > 65536)
            return PSN_BACKEND_UNSUPPORTED;
        job->request["bodyHex"] = hex(r->body);
        return submit(*static_cast<Instance *>(opaque), job, out);
    });
}
int32_t web_poll(void *opaque, uint64_t id, PsnWebResponse *out) {
    if (!opaque || !out)
        return PSN_INVALID_ARGUMENT;
    auto &i = *static_cast<Instance *>(opaque);
    std::lock_guard lock(i.mutex);
    auto found = i.jobs.find(id);
    if (found == i.jobs.end() || found->second->kind != Job::web)
        return PSN_INVALID_ARGUMENT;
    auto &job = *found->second;
    if (!job.done)
        return 1;
    trace_web_completion(i, job);
    *out = {};
    out->result = job.result;
    out->http_status = job.http;
    job.header = {"Content-Type", job.content_type.c_str()};
    if (!job.result) {
        out->header_count = 1;
        out->headers = &job.header;
        out->body = {job.body.data(), job.body.size()};
    }
    return 0;
}
void cancel(void *opaque, uint64_t id) {
    if (!opaque)
        return;
    auto &i = *static_cast<Instance *>(opaque);
    std::lock_guard lock(i.mutex);
    auto found = i.jobs.find(id);
    if (found == i.jobs.end() || found->second->done)
        return;
    auto &job = *found->second;
    job.canceled = true;
    job.done = true;
    if (i.trace_timing)
        job.completed_at = Clock::now();
    job.result = job.auth_result.result = PSN_BACKEND_ABORTED;
    i.wake.notify_all();
}
void release(void *opaque, uint64_t id) {
    if (!opaque)
        return;
    cancel(opaque, id);
    auto &i = *static_cast<Instance *>(opaque);
    std::unique_lock lock(i.mutex);
    auto found = i.jobs.find(id);
    if (found == i.jobs.end())
        return;
    auto job = found->second;
    i.wake.wait(lock, [&] { return !job->active; });
    i.jobs.erase(id);
    std::erase(i.pending, id);
}
int32_t subscribe(void *opaque, uint64_t cookie, const PsnPushFilter *f) {
    if (!opaque || !f || !cookie || f->user_id != static_cast<Instance *>(opaque)->local_user_id ||
        f->kind < 1 || f->kind > 3 || f->ext_key_count > PSN_MAX_EXT_DATA ||
        (f->ext_key_count && !f->ext_keys))
        return PSN_INVALID_ARGUMENT;
    return guarded([&]() -> int32_t {
        Subscription s{f->kind, f->service_label, f->service != nullptr,
                       input(f->service ? f->service : "", 255), input(f->data_type, 64, false)};
        for (uint32_t n = 0; n < f->ext_key_count; ++n)
            input(f->ext_keys[n], 32, false);
        auto &i = *static_cast<Instance *>(opaque);
        std::lock_guard lock(i.mutex);
        if (i.failure)
            return i.failure;
        if (i.subscriptions.size() >= 512 || i.subscriptions.contains(cookie))
            return PSN_BACKEND_BUSY;
        i.subscriptions.emplace(cookie, std::move(s));
        return 0;
    });
}
void unsubscribe(void *opaque, uint64_t cookie) {
    if (!opaque)
        return;
    auto &i = *static_cast<Instance *>(opaque);
    std::lock_guard lock(i.mutex);
    i.subscriptions.erase(cookie);
    std::erase_if(i.events, [&](const auto &e) {
        return e->value.kind == 2 && e->value.data.push.subscription == cookie;
    });
}
int32_t event_next(void *opaque, PsnEvent *out) {
    if (!opaque || !out)
        return PSN_INVALID_ARGUMENT;
    return guarded([&]() -> int32_t {
        auto &i = *static_cast<Instance *>(opaque);
        std::lock_guard lock(i.mutex);
        i.borrowed.reset();
        if (i.failure)
            return i.failure;
        if (i.events.empty())
            return 0;
        i.borrowed = i.events.front();
        i.events.pop_front();
        i.borrowed->bind();
        *out = i.borrowed->value;
        if (i.trace_timing && out->kind == 3) {
            const auto now = Clock::now();
            std::fprintf(stderr,
                "PSN timing: invitation_to_host t_ms=%lld queue_ms=%lld\n",
                milliseconds(i.timing_origin, now), milliseconds(i.borrowed->received_at, now));
        }
        return 1;
    });
}
int32_t invitation_start(void *opaque, const PsnInvitationRequest *r, uint64_t *out) {
    if (!opaque || !r || !out || r->user_id != static_cast<Instance *>(opaque)->local_user_id)
        return PSN_INVALID_ARGUMENT;
    if (r->mode != 1 || r->user_count != 1 || r->max_selectable_users)
        return PSN_BACKEND_UNSUPPORTED;
    if (!r->users)
        return PSN_INVALID_ARGUMENT;
    return guarded([&]() -> int32_t {
        if (!input(r->message ? r->message : "", 512).empty())
            return PSN_BACKEND_UNSUPPORTED;
        auto job = std::make_shared<Job>();
        job->kind = Job::invitation;
        job->session = input(r->session_id, 45, false);
        // The provider's session identifiers are canonical decimal uint64 IDs;
        // validating before path construction also excludes URL/path injection.
        decimal(job->session);
        job->recipient.account_id = r->users[0].account_id;
        if (!job->recipient.account_id)
            copy(job->recipient.online_id, input(r->users[0].online_id, 16, false));
        return submit(*static_cast<Instance *>(opaque), job, out);
    });
}
int32_t invitation_poll(void *opaque, uint64_t id, PsnInvitationResult *out) {
    if (!opaque || !out)
        return PSN_INVALID_ARGUMENT;
    auto &i = *static_cast<Instance *>(opaque);
    std::lock_guard lock(i.mutex);
    auto found = i.jobs.find(id);
    if (found == i.jobs.end() || found->second->kind != Job::invitation)
        return PSN_INVALID_ARGUMENT;
    const auto &job = *found->second;
    if (!job.done)
        return 1;
    *out = {};
    if (job.result) {
        out->error_code = job.result;
        out->result = 1;
    } else {
        *out = job.invitation_result;
    }
    return 0;
}
const PsnBackendV1 backend{sizeof(PsnBackendV1),
                           PSN_BACKEND_ABI_V1,
                           PSN_PROFILE_SOCIAL_V1,
                           open,
                           close,
                           identity,
                           policy,
                           title,
                           restriction,
                           auth_start,
                           auth_poll,
                           cancel,
                           release,
                           web_start,
                           web_poll,
                           cancel,
                           release,
                           subscribe,
                           unsubscribe,
                           event_next,
                           invitation_start,
                           invitation_poll,
                           cancel,
                           release};
} // namespace
#ifdef _WIN32
#define PSN_EXPORT __declspec(dllexport)
#else
#define PSN_EXPORT __attribute__((visibility("default")))
#endif
extern "C" PSN_EXPORT const PsnBackendV1 *psn_backend_query(uint32_t version) {
    return version == PSN_BACKEND_ABI_V1 ? &backend : nullptr;
}
