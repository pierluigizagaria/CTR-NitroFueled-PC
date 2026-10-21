/* SPDX-License-Identifier: GPL-2.0-or-later */
extern "C" {
#include "../psn_host.h"
}
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
using namespace std::chrono_literals;
static void environment(const char *key, const std::string &value) {
#ifdef _WIN32
    assert(!_putenv_s(key, value.c_str()));
#else
    assert(!setenv(key, value.c_str(), 1));
#endif
}
static uint64_t start(const char *path, uint32_t method) {
    void *instance;
    const auto *b = psn_host_lock(&instance);
    const char body[] = "private-timing-payload";
    const PsnHeader header{"X-Private-Timing", "private-timing-header"};
    PsnWebRequest request{};
    request.user_id = 1;
    request.method = method;
    request.api_group = "private-timing-group";
    request.path = path;
    request.content_type = "application/octet-stream";
    request.headers = &header;
    request.header_count = 1;
    request.body = {body, sizeof(body) - 1};
    uint64_t handle = 0;
    assert(!b->web_start(instance, &request, &handle));
    psn_host_unlock();
    return handle;
}
static void complete(uint64_t handle) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    for (;;) {
        void *instance;
        const auto *b = psn_host_lock(&instance);
        PsnWebResponse response{};
        const auto result = b->web_poll(instance, handle, &response);
        if (!result) {
            assert(!response.result && response.http_status == 200 &&
                   std::string(static_cast<const char *>(response.body.data), response.body.size) ==
                       "private-timing-response");
            // Re-reading a retained result must not duplicate a completion record.
            assert(!b->web_poll(instance, handle, &response));
            b->web_release(instance, handle);
            psn_host_unlock();
            return;
        }
        psn_host_unlock();
        assert(result == 1 && std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(5ms);
    }
}
int main(int argc, char **argv) {
    assert(argc == 4);
    environment("BB_PSN_BACKEND", argv[1]);
    environment("BB_PSN_BACKEND_CONFIG",
                nlohmann::json({{"url", argv[2]}, {"token", argv[3]}}).dump());
    const int32_t user = 1;
    assert(!psn_host_set_local_users(&user, 1) && !psn_host_start());
    const auto first = start("/v1/sessions/76561198099999999/members?private-timing-query", 1);
    const auto second = start("/v1/sessions/76561198099999999/sessionData?private-timing-query", 0);
    // The server delays the first response. The second request waits in the
    // backend queue, while the first retained result is consumed later.
    complete(second);
    std::this_thread::sleep_for(200ms);
    complete(first);
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!psn_host_system_count()) {
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(5ms);
    }
    assert(psn_host_system_count() == 1);
    unsigned char event[180]{};
    assert(!psn_host_system_receive(event) && psn_host_system_count() == 0);
    psn_host_stop();
}
