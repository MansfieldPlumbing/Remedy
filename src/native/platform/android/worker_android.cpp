#include "remedy/ports/worker_port.h"
#include "channel_worker_android.h"

#include <errno.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <mutex>
#include <new>
#include <thread>
#include <unordered_map>

extern char** environ;

static constexpr int REMEDY_WORKER_CHANNEL_FD = 3;

struct android_worker_entry {
    pid_t pid{-1};
    bool reaped{false};
};

static std::mutex g_worker_mutex;
static std::unordered_map<remedy_worker_token_t, android_worker_entry*> g_worker_table;
static remedy_worker_token_t g_next_worker_token = 1;

#ifdef REMEDY_TEST_INHERITED_CHANNEL_SEAM
static int g_test_last_bootstrap_descriptor = -1;
extern "C" int remedy_test_get_last_bootstrap_descriptor(void) {
    return g_test_last_bootstrap_descriptor;
}
#endif

static remedy_err_t observe_death(android_worker_entry* entry, uint32_t timeout_ms, bool* out_died) {
    *out_died = false;
    if (entry->reaped) {
        *out_died = true;
        return REMEDY_OK;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        int status = 0;
        pid_t result = waitpid(entry->pid, &status, WNOHANG);
        if (result == entry->pid) {
            entry->reaped = true;
            *out_died = true;
            return REMEDY_OK;
        }
        if (result < 0) return REMEDY_ERR_WAIT_FAILED;
        if (std::chrono::steady_clock::now() >= deadline) return REMEDY_ERR_TIMEOUT;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

extern "C" remedy_err_t worker_port_start(
    const remedy_worker_config_t* config,
    remedy_worker_token_t* out_token) {
    if (!out_token) return REMEDY_ERR_INVALID_ARGUMENT;
    *out_token = REMEDY_INVALID_WORKER_TOKEN;
    if (!config || !config->executable_path || config->executable_path[0] != '/') {
        return REMEDY_ERR_INVALID_ARGUMENT;
    }
    if (config->arguments && config->arguments[0] != '\0') return REMEDY_ERR_NOT_SUPPORTED;
    if (config->working_directory && config->working_directory[0] != '\0') return REMEDY_ERR_NOT_SUPPORTED;
    if (config->timeout_ms != 0) return REMEDY_ERR_NOT_SUPPORTED;

    int endpoint = -1;
    remedy_err_t issue_result = channel_android_issue_worker_endpoint(config->bootstrap_channel, &endpoint);
    if (issue_result != REMEDY_OK) return issue_result;
#ifdef REMEDY_TEST_INHERITED_CHANNEL_SEAM
    g_test_last_bootstrap_descriptor = endpoint;
#endif

    posix_spawnattr_t attributes{};
    posix_spawn_file_actions_t actions{};
    bool attributes_initialized = false;
    bool actions_initialized = false;
    remedy_err_t result = REMEDY_OK;

    int call_result = posix_spawnattr_init(&attributes);
    if (call_result == 0) attributes_initialized = true;
    else result = REMEDY_ERR_CONTAINMENT_FAILED;
    if (result == REMEDY_OK) {
        call_result = posix_spawnattr_setflags(&attributes, POSIX_SPAWN_CLOEXEC_DEFAULT);
        if (call_result != 0) result = REMEDY_ERR_CONTAINMENT_FAILED;
    }
    if (result == REMEDY_OK) {
        call_result = posix_spawn_file_actions_init(&actions);
        if (call_result == 0) actions_initialized = true;
        else result = REMEDY_ERR_CONTAINMENT_FAILED;
    }
    if (result == REMEDY_OK) {
        call_result = posix_spawn_file_actions_adddup2(&actions, endpoint, REMEDY_WORKER_CHANNEL_FD);
        if (call_result != 0) result = REMEDY_ERR_CONTAINMENT_FAILED;
    }
    if (result == REMEDY_OK && endpoint != REMEDY_WORKER_CHANNEL_FD) {
        call_result = posix_spawn_file_actions_addclose(&actions, endpoint);
        if (call_result != 0) result = REMEDY_ERR_CONTAINMENT_FAILED;
    }

    pid_t pid = -1;
    if (result == REMEDY_OK) {
        char* const argv[] = {const_cast<char*>(config->executable_path), nullptr};
        call_result = posix_spawn(&pid, config->executable_path, &actions, &attributes, argv, environ);
        if (call_result != 0) result = REMEDY_ERR_IPC_FAILURE;
    }

    bool cleanup_ok = true;
    if (actions_initialized) cleanup_ok = posix_spawn_file_actions_destroy(&actions) == 0;
    if (attributes_initialized) cleanup_ok = (posix_spawnattr_destroy(&attributes) == 0) && cleanup_ok;
    cleanup_ok = (close(endpoint) == 0) && cleanup_ok;
    if (!cleanup_ok) {
        if (pid > 0) {
            bool killed = kill(pid, SIGKILL) == 0 || errno == ESRCH;
            int status = 0;
            bool reaped = waitpid(pid, &status, 0) == pid;
            if (!killed || !reaped) return REMEDY_ERR_CONTAINMENT_FAILED;
        }
        return REMEDY_ERR_CONTAINMENT_FAILED;
    }
    if (result != REMEDY_OK) return result;

    auto* entry = new (std::nothrow) android_worker_entry{pid, false};
    if (!entry) {
        bool killed = kill(pid, SIGKILL) == 0 || errno == ESRCH;
        int status = 0;
        bool reaped = waitpid(pid, &status, 0) == pid;
        return (killed && reaped) ? REMEDY_ERR_OUT_OF_MEMORY : REMEDY_ERR_CONTAINMENT_FAILED;
    }

    std::lock_guard<std::mutex> lock(g_worker_mutex);
    remedy_worker_token_t token = g_next_worker_token++;
    if (token == REMEDY_INVALID_WORKER_TOKEN || g_worker_table.find(token) != g_worker_table.end()) {
        bool killed = kill(pid, SIGKILL) == 0 || errno == ESRCH;
        int status = 0;
        bool reaped = waitpid(pid, &status, 0) == pid;
        delete entry;
        return (killed && reaped) ? REMEDY_ERR_OUT_OF_MEMORY : REMEDY_ERR_CONTAINMENT_FAILED;
    }
    try {
        auto insertion = g_worker_table.emplace(token, entry);
        if (!insertion.second) {
            bool killed = kill(pid, SIGKILL) == 0 || errno == ESRCH;
            int status = 0;
            bool reaped = waitpid(pid, &status, 0) == pid;
            delete entry;
            return (killed && reaped) ? REMEDY_ERR_OUT_OF_MEMORY : REMEDY_ERR_CONTAINMENT_FAILED;
        }
    } catch (...) {
        bool killed = kill(pid, SIGKILL) == 0 || errno == ESRCH;
        int status = 0;
        bool reaped = waitpid(pid, &status, 0) == pid;
        delete entry;
        return (killed && reaped) ? REMEDY_ERR_OUT_OF_MEMORY : REMEDY_ERR_CONTAINMENT_FAILED;
    }
    *out_token = token;
    return REMEDY_OK;
}

extern "C" remedy_err_t worker_port_wait_for_death(
    remedy_worker_token_t token,
    uint32_t timeout_ms,
    bool* out_died) {
    if (!out_died) return REMEDY_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(g_worker_mutex);
    auto it = g_worker_table.find(token);
    if (it == g_worker_table.end()) return REMEDY_ERR_INVALID_ARGUMENT;
    return observe_death(it->second, timeout_ms, out_died);
}

extern "C" remedy_err_t worker_port_terminate(remedy_worker_token_t token) {
    std::lock_guard<std::mutex> lock(g_worker_mutex);
    auto it = g_worker_table.find(token);
    if (it == g_worker_table.end()) return REMEDY_ERR_INVALID_ARGUMENT;
    android_worker_entry* entry = it->second;
    if (entry->reaped) return REMEDY_OK;
    if (kill(entry->pid, SIGKILL) != 0 && errno != ESRCH) return REMEDY_ERR_WORKER_TERMINATION_FAILED;
    bool died = false;
    remedy_err_t wait_result = observe_death(entry, 2000, &died);
    return (wait_result == REMEDY_OK && died) ? REMEDY_OK : REMEDY_ERR_WORKER_TERMINATION_FAILED;
}

extern "C" remedy_err_t worker_port_destroy(remedy_worker_token_t token) {
    std::lock_guard<std::mutex> lock(g_worker_mutex);
    auto it = g_worker_table.find(token);
    if (it == g_worker_table.end()) return REMEDY_ERR_INVALID_ARGUMENT;
    if (!it->second->reaped) return REMEDY_ERR_INVALID_ARGUMENT;
    delete it->second;
    g_worker_table.erase(it);
    return REMEDY_OK;
}
