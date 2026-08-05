#include "remedy/ports/worker_port.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <thread>
#include <stdio.h>

enum worker_token_state {
    WORKER_TOKEN_LIVE    = 1,
    WORKER_TOKEN_CLOSING = 2,
    WORKER_TOKEN_RETIRED = 3
};

struct win32_worker_entry {
    HANDLE process_handle{NULL};
    HANDLE thread_handle{NULL};
    HANDLE job_handle{NULL};
    DWORD  process_id{0};
    std::atomic<uint32_t> state{WORKER_TOKEN_LIVE};
    std::atomic<uint32_t> op_count{0};
};

class worker_op_lease {
public:
    explicit worker_op_lease(win32_worker_entry* entry) : entry_(entry) {
        if (entry_) {
            entry_->op_count.fetch_add(1, std::memory_order_relaxed);
            if (entry_->state.load(std::memory_order_acquire) != WORKER_TOKEN_LIVE) {
                entry_->op_count.fetch_sub(1, std::memory_order_release);
                entry_ = nullptr;
            }
        }
    }

    ~worker_op_lease() {
        if (entry_) {
            entry_->op_count.fetch_sub(1, std::memory_order_release);
        }
    }

    explicit operator bool() const { return entry_ != nullptr; }
    win32_worker_entry* get() const { return entry_; }

private:
    win32_worker_entry* entry_{nullptr};
};

static std::mutex g_worker_mutex;
static std::unordered_map<remedy_worker_token_t, win32_worker_entry*> g_worker_table;
static std::atomic<uint64_t> g_next_worker_token{1};

extern "C" {

remedy_err_t worker_port_start(const remedy_worker_config_t* config, remedy_worker_token_t* out_token) {
    if (!config || !out_token || !config->executable_path) return REMEDY_ERR_INVALID_ARGUMENT;

    wchar_t wCmd[1024] = { 0 };
    if (config->channel_nonce && config->arguments) {
        char fullArgs[1024];
        snprintf(fullArgs, sizeof(fullArgs), "\"%s\" %s --channel=%s", config->executable_path, config->arguments, config->channel_nonce);
        MultiByteToWideChar(CP_UTF8, 0, fullArgs, -1, wCmd, 1024);
    } else if (config->channel_nonce) {
        char fullArgs[512];
        snprintf(fullArgs, sizeof(fullArgs), "\"%s\" --channel=%s", config->executable_path, config->channel_nonce);
        MultiByteToWideChar(CP_UTF8, 0, fullArgs, -1, wCmd, 512);
    } else if (config->arguments) {
        char fullArgs[512];
        snprintf(fullArgs, sizeof(fullArgs), "\"%s\" %s", config->executable_path, config->arguments);
        MultiByteToWideChar(CP_UTF8, 0, fullArgs, -1, wCmd, 512);
    } else {
        char fullArgs[512];
        snprintf(fullArgs, sizeof(fullArgs), "\"%s\"", config->executable_path);
        MultiByteToWideChar(CP_UTF8, 0, fullArgs, -1, wCmd, 512);
    }

    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = { 0 };

    // 1. Create process SUSPENDED
    BOOL procSuccess = CreateProcessW(
        NULL,
        wCmd,
        NULL, NULL, FALSE,
        CREATE_SUSPENDED | CREATE_NO_WINDOW,
        NULL, NULL, &si, &pi
    );

    if (!procSuccess) return REMEDY_ERR_IPC_FAILURE;

    // 2. Create Job Object with JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE guarantee
    HANDLE hJob = CreateJobObjectW(NULL, NULL);
    if (!hJob) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return REMEDY_ERR_CONTAINMENT_FAILED;
    }

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli = { 0 };
    jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(hJob, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli))) {
        CloseHandle(hJob);
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return REMEDY_ERR_CONTAINMENT_FAILED;
    }

    // 3. Assign process to Job Object
    if (!AssignProcessToJobObject(hJob, pi.hProcess)) {
        CloseHandle(hJob);
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return REMEDY_ERR_CONTAINMENT_FAILED;
    }

    // 4. Resume thread ONLY after containment fully succeeded
    if (ResumeThread(pi.hThread) == (DWORD)-1) {
        CloseHandle(hJob);
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return REMEDY_ERR_CONTAINMENT_FAILED;
    }

    win32_worker_entry* entry = new win32_worker_entry();
    entry->process_handle = pi.hProcess;
    entry->thread_handle = pi.hThread;
    entry->job_handle = hJob;
    entry->process_id = pi.dwProcessId;

    remedy_worker_token_t token = g_next_worker_token.fetch_add(1, std::memory_order_relaxed);

    {
        std::lock_guard<std::mutex> lock(g_worker_mutex);
        g_worker_table[token] = entry;
    }

    *out_token = token;
    return REMEDY_OK;
}

remedy_err_t worker_port_request_quiescence(remedy_worker_token_t token) {
    return REMEDY_ERR_NOT_SUPPORTED;
}

remedy_err_t worker_port_terminate(remedy_worker_token_t token) {
    win32_worker_entry* raw_entry = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_worker_mutex);
        auto it = g_worker_table.find(token);
        if (it == g_worker_table.end()) return REMEDY_ERR_INVALID_ARGUMENT;
        raw_entry = it->second;
    }

    worker_op_lease lease(raw_entry);
    if (!lease) return REMEDY_ERR_HANDLE_STALE;
    win32_worker_entry* entry = lease.get();

    if (entry->job_handle) {
        // Closing job handle forces JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE on entire descendant tree
        CloseHandle(entry->job_handle);
        entry->job_handle = NULL;
    } else if (entry->process_handle) {
        TerminateProcess(entry->process_handle, 1);
    }
    return REMEDY_OK;
}

remedy_err_t worker_port_wait_for_death(remedy_worker_token_t token, uint32_t timeout_ms, bool* out_died) {
    win32_worker_entry* raw_entry = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_worker_mutex);
        auto it = g_worker_table.find(token);
        if (it == g_worker_table.end()) return REMEDY_ERR_INVALID_ARGUMENT;
        raw_entry = it->second;
    }

    worker_op_lease lease(raw_entry);
    if (!lease) return REMEDY_ERR_HANDLE_STALE;
    win32_worker_entry* entry = lease.get();

    if (out_died) *out_died = false;
    if (!entry->process_handle) return REMEDY_ERR_INVALID_ARGUMENT;

    DWORD res = WaitForSingleObject(entry->process_handle, timeout_ms);
    if (res == WAIT_OBJECT_0) {
        if (out_died) *out_died = true;
        return REMEDY_OK;
    } else if (res == WAIT_TIMEOUT) {
        if (out_died) *out_died = false;
        return REMEDY_ERR_TIMEOUT;
    } else {
        if (out_died) *out_died = false;
        return REMEDY_ERR_WAIT_FAILED;
    }
}

remedy_err_t worker_port_destroy(remedy_worker_token_t token) {
    win32_worker_entry* entry = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_worker_mutex);
        auto it = g_worker_table.find(token);
        if (it == g_worker_table.end()) return REMEDY_ERR_INVALID_ARGUMENT;
        entry = it->second;
        g_worker_table.erase(it);
    }

    // Mark CLOSING
    entry->state.store(WORKER_TOKEN_CLOSING, std::memory_order_release);

    // Wait for active operation pins to drain
    while (entry->op_count.load(std::memory_order_acquire) > 0) {
        std::this_thread::yield();
    }

    if (entry->job_handle) CloseHandle(entry->job_handle);
    if (entry->thread_handle) CloseHandle(entry->thread_handle);
    if (entry->process_handle) CloseHandle(entry->process_handle);

    entry->state.store(WORKER_TOKEN_RETIRED, std::memory_order_release);
    delete entry;
    return REMEDY_OK;
}

} // extern "C"
