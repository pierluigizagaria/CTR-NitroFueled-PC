/* SPDX-License-Identifier: GPL-2.0-or-later */
extern "C" {
#include "../psn_host.h"
}
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
using namespace std::chrono_literals;
constexpr uint64_t recipient = UINT64_C(76561198000000002);

static void environment(const char *key, const std::string &value) {
#ifdef _WIN32
    assert(!_putenv_s(key, value.c_str()));
#else
    assert(!setenv(key, value.c_str(), 1));
#endif
}
static void pause() { std::this_thread::sleep_for(5ms); }
static void wait_stage(const char *path, const char *expected) {
    for (unsigned n = 0; n < 1000; ++n) {
        std::ifstream file(path);
        std::string stage;
        if (file >> stage && stage == expected)
            return;
        pause();
    }
    assert(false);
}
template <class T> T symbol(const char *name) {
    auto value = psn_host_resolve(name);
    assert(value);
    return reinterpret_cast<T>(value);
}

// Guest layouts exercise the real host conversion and its asynchronous result,
// independently from the direct backend tests below.
struct GuestSession { char data[46], padding[2]; };
struct GuestAddress {
    int32_t type, padding;
    struct { const uint64_t *users; uint32_t count, reserved; } fixed;
};
struct GuestSend {
    const char *message;
    const GuestSession *session;
    GuestAddress address;
    uint8_t reserved[24];
};
struct GuestParam {
    uint64_t base_size;
    uint8_t base_reserved[36];
    uint32_t magic, size;
    int32_t mode, user;
    uint32_t padding;
    void *callback_argument;
    const GuestSend *data;
    uint8_t reserved[64];
};
struct GuestUser { char online[20]; uint32_t padding; uint64_t account; };
struct GuestUsers { uint32_t count, padding; GuestUser users[16]; };
struct GuestResult {
    void *argument;
    int32_t error, result;
    GuestUsers *users;
    uint8_t reserved[32];
};
static_assert(sizeof(GuestParam) == 144 && sizeof(GuestResult) == 56 &&
              sizeof(GuestSend) == 64 && sizeof(GuestUser) == 32);
using NoArg = int32_t(PSN_GUEST_ABI *)();
using Open = int32_t(PSN_GUEST_ABI *)(const GuestParam *);
using Result = int32_t(PSN_GUEST_ABI *)(GuestResult *);

int main(int argc, char **argv) {
    assert(argc == 5);
    const std::string mode = argv[3];
    const int32_t user = mode == "zero-user" ? 0 : 70001;
    environment("BB_PSN_BACKEND", argv[1]);
    environment("BB_PSN_BACKEND_CONFIG", nlohmann::json({{"url", argv[2]},
        {"token", std::string(64, 'a')}, {"local_user_id", user}}).dump());
    assert(!psn_host_set_local_users(&user, 1) && !psn_host_start());
    char session[] = "76561198099999999";
    PsnInvitationUser selected{};
    selected.account_id = mode == "unknown" ? recipient + 100 : recipient;
    const bool legacy = mode == "legacy" || mode == "numeric-name" || mode == "ambiguous";
    if (legacy) {
        selected.account_id = 0;
        std::strcpy(selected.online_id, mode == "numeric-name" ? "123456789" : "Friend");
    }
    PsnInvitationRequest request{user, 1, session, "", &selected, 1, 0};

    if (mode == "guest") {
        GuestSession guest_session{};
        std::strcpy(guest_session.data, session);
        uint64_t account = recipient;
        GuestSend send{"", &guest_session, {1, 0, {&account, 1, 0}}, {}};
        GuestParam param{};
        param.base_size = 48; param.size = sizeof(param); param.mode = 1; param.user = user;
        int marker = 42;
        param.callback_argument = &marker; param.data = &send;
        assert(!symbol<NoArg>("sceInvitationDialogInitialize")());
        assert(!symbol<Open>("sceInvitationDialogOpenA")(&param));
        account = 0;
        std::memset(guest_session.data, 'X', sizeof(guest_session.data));
        GuestUsers users{};
        GuestResult result{};
        result.users = &users;
        assert(symbol<Result>("sceInvitationDialogGetResultA")(&result) != 0);
        for (unsigned n = 0; n < 1000 && symbol<NoArg>("sceInvitationDialogUpdateStatus")() == 2; ++n)
            pause();
        assert(symbol<NoArg>("sceInvitationDialogGetStatus")() == 3);
        assert(!symbol<Result>("sceInvitationDialogGetResultA")(&result));
        assert(result.argument == &marker && !result.error && !result.result && users.count == 1);
        assert(users.users[0].account == recipient && !strcmp(users.users[0].online, "Friend"));
        assert(!symbol<NoArg>("sceInvitationDialogTerminate")());
    } else {
        void *instance;
        const auto *backend = psn_host_lock(&instance);
        uint64_t handle = 0;
        if (mode == "invalid") {
            assert(backend->invitation_start(instance, nullptr, &handle) == PSN_INVALID_ARGUMENT);
            request.user_id = user + 1;
            assert(backend->invitation_start(instance, &request, &handle) == PSN_INVALID_ARGUMENT);
            request.user_id = user;
            for (const char *invalid : {"", "0", "01", "-1", "18446744073709551616", "1/invitations", "1?x=2"}) {
                request.session_id = invalid;
                assert(backend->invitation_start(instance, &request, &handle) == PSN_INVALID_ARGUMENT);
            }
            request.session_id = session;
            request.message = "unsupported message";
            assert(backend->invitation_start(instance, &request, &handle) == PSN_BACKEND_UNSUPPORTED);
            request.message = "";
            request.user_count = 2;
            assert(backend->invitation_start(instance, &request, &handle) == PSN_BACKEND_UNSUPPORTED);
            request.user_count = 0; request.max_selectable_users = 1;
            assert(backend->invitation_start(instance, &request, &handle) == PSN_BACKEND_UNSUPPORTED);
            request.mode = 2;
            assert(backend->invitation_start(instance, &request, &handle) == PSN_BACKEND_UNSUPPORTED);
            psn_host_unlock();
        } else {
            const auto started = std::chrono::steady_clock::now();
            assert(!backend->invitation_start(instance, &request, &handle));
            assert(std::chrono::steady_clock::now() - started < 250ms);
            std::memset(session, 'X', sizeof(session));
            selected = {}; // The queued operation must own both input copies.
            PsnInvitationResult result{};
            assert(backend->invitation_poll(instance, handle, &result) == 1);
            PsnWebResponse wrong{};
            assert(backend->web_poll(instance, handle, &wrong) == PSN_INVALID_ARGUMENT);
            psn_host_unlock();
            const bool canceled = mode == "cancel-lookup" || mode == "cancel-send";
            if (canceled) {
                wait_stage(argv[4], mode == "cancel-lookup" ? "lookup" : "send");
                backend = psn_host_lock(&instance);
                const auto cancel_start = std::chrono::steady_clock::now();
                backend->invitation_cancel(instance, handle);
                assert(!backend->invitation_poll(instance, handle, &result));
                assert(result.error_code == PSN_BACKEND_ABORTED && result.result == 1 && !result.user_count);
                backend->invitation_release(instance, handle);
                psn_host_unlock();
                assert(std::chrono::steady_clock::now() - cancel_start < 2500ms);
            } else {
                bool complete = false;
                for (unsigned n = 0; n < 1200; ++n) {
                    backend = psn_host_lock(&instance);
                    const auto rc = backend->invitation_poll(instance, handle, &result);
                    psn_host_unlock();
                    if (!rc) { complete = true; break; }
                    assert(rc == 1);
                    pause();
                }
                assert(complete);
                const bool success = mode == "account" || mode == "legacy" || mode == "numeric-name" || mode == "zero-user";
                if (success) {
                    assert(!result.error_code && !result.result && result.user_count == 1);
                    assert(result.users[0].account_id == recipient);
                    assert(!strcmp(result.users[0].online_id, mode == "numeric-name" ? "123456789" : "Friend"));
                } else {
                    assert(result.error_code && result.result == 1 && !result.user_count);
                }
                backend = psn_host_lock(&instance);
                PsnInvitationResult again{};
                assert(!backend->invitation_poll(instance, handle, &again));
                assert(!memcmp(&result, &again, sizeof(result)));
                backend->invitation_release(instance, handle);
                assert(backend->invitation_poll(instance, handle, &again) == PSN_INVALID_ARGUMENT);
                psn_host_unlock();
            }
        }
    }
    psn_host_stop();
    puts("Fixed-recipient invitation contract passed");
}
