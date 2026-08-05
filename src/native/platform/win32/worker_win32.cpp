#include "remedy/ports/worker_port.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <unordered_map>
#include <mutex>
#include <new>
#include <cstdlib>
#include <cwchar>
#include <string>

struct win32_worker_entry {
    HANDLE process_handle{NULL};
    HANDLE thread_handle{NULL};
    HANDLE job_handle{NULL};
    DWORD  process_id{0};
};

static std::mutex g_worker_mutex;
static std::unordered_map<remedy_worker_token_t, win32_worker_entry*> g_worker_table;
static remedy_worker_token_t g_next_worker_token = 1;

#ifdef REMEDY_TEST_STAGED_FAILURE_SEAM
static int g_test_fail_stage = 0;
static HANDLE g_test_observer_handle = NULL;

extern "C" {
void remedy_test_set_fail_stage(int stage) {
    g_test_fail_stage = stage;
}

HANDLE remedy_test_take_observer_handle(void) {
    HANDLE h = g_test_observer_handle;
    g_test_observer_handle = NULL;
    return h;
}

size_t remedy_test_get_registry_size(void) {
    std::lock_guard<std::mutex> lock(g_worker_mutex);
    return g_worker_table.size();
}
}
#endif

[[noreturn]] static void remedy_fail_stop_containment_breach(HANDLE& hProcess, HANDLE& hThread, HANDLE& hJob) {
    while (hProcess != NULL) {
        DWORD waitRes = WaitForSingleObject(hProcess, 100);
        if (waitRes == WAIT_OBJECT_0) {
            break;
        }
        if (hJob != NULL) {
            if (CloseHandle(hJob)) {
                hJob = NULL;
            }
        }
        if (hProcess != NULL) {
            BOOL termOk = TerminateProcess(hProcess, 1);
            if (termOk) {
                waitRes = WaitForSingleObject(hProcess, 100);
                if (waitRes == WAIT_OBJECT_0) {
                    break;
                }
            }
        }
        Sleep(10);
    }

    while (hJob != NULL || hThread != NULL || hProcess != NULL) {
        if (hJob != NULL) {
            if (CloseHandle(hJob)) {
                hJob = NULL;
            }
        }
        if (hThread != NULL) {
            if (CloseHandle(hThread)) {
                hThread = NULL;
            }
        }
        if (hProcess != NULL) {
            if (CloseHandle(hProcess)) {
                hProcess = NULL;
            }
        }
        if (hJob != NULL || hThread != NULL || hProcess != NULL) {
            Sleep(10);
        }
    }

    std::abort();
}

static void do_verified_failure_cleanup(HANDLE& hProcess, HANDLE& hThread, HANDLE& hJob, bool assigned_to_job) {
    bool term_requested = false;
    if (assigned_to_job && hJob != NULL) {
        if (CloseHandle(hJob)) {
            hJob = NULL;
            term_requested = true;
        }
    } else {
        if (hProcess != NULL) {
            BOOL termOk = TerminateProcess(hProcess, 1);
            if (termOk) {
                term_requested = true;
            }
        }
    }

    if (hProcess != NULL) {
        DWORD waitRes = WaitForSingleObject(hProcess, 2000);
        if (waitRes != WAIT_OBJECT_0 || !term_requested) {
            if (waitRes != WAIT_OBJECT_0) {
                remedy_fail_stop_containment_breach(hProcess, hThread, hJob);
            }
        }
    }

    if (hThread != NULL) {
        if (CloseHandle(hThread)) {
            hThread = NULL;
        } else {
            remedy_fail_stop_containment_breach(hProcess, hThread, hJob);
        }
    }

    if (hProcess != NULL) {
        if (CloseHandle(hProcess)) {
            hProcess = NULL;
        } else {
            remedy_fail_stop_containment_breach(hProcess, hThread, hJob);
        }
    }

    if (hJob != NULL) {
        if (CloseHandle(hJob)) {
            hJob = NULL;
        } else {
            remedy_fail_stop_containment_breach(hProcess, hThread, hJob);
        }
    }
}

static bool is_valid_absolute_path(const char* path) {
    if (!path || path[0] == '\0') return false;
    if (path[1] == '\0') return false;

    // Drive-rooted: requires at least 3 chars (path[0], path[1], path[2])
    if (((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) && path[1] == ':') {
        if (path[2] == '/' || path[2] == '\\') {
            return true;
        }
        return false;
    }

    // UNC or Extended path: requires at least 2 chars (path[0], path[1])
    if (path[0] == '\\' && path[1] == '\\') {
        return true;
    }

    return false;
}

static bool convert_utf8_to_wide(const char* utf8_str, std::wstring& out_wide) {
    if (!utf8_str) return false;
    int req_chars = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8_str, -1, NULL, 0);
    if (req_chars <= 0) return false;

    out_wide.resize(req_chars);
    int converted = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8_str, -1, &out_wide[0], req_chars);
    if (converted != req_chars) return false;
    if (out_wide[req_chars - 1] != L'\0') return false;

    out_wide.resize(req_chars - 1);
    return true;
}

static remedy_worker_token_t generate_unique_token_locked() {
    remedy_worker_token_t start_token = g_next_worker_token;
    do {
        remedy_worker_token_t token = g_next_worker_token++;
        if (token == REMEDY_INVALID_WORKER_TOKEN) {
            token = g_next_worker_token++;
        }
        if (g_worker_table.find(token) == g_worker_table.end()) {
            return token;
        }
    } while (g_next_worker_token != start_token);
    return REMEDY_INVALID_WORKER_TOKEN;
}

extern "C" {

remedy_err_t worker_port_start(const remedy_worker_config_t* config, remedy_worker_token_t* out_token) {
    if (!out_token) return REMEDY_ERR_INVALID_ARGUMENT;
    *out_token = REMEDY_INVALID_WORKER_TOKEN;

    if (!config) return REMEDY_ERR_INVALID_ARGUMENT;
    if (!config->executable_path || config->executable_path[0] == '\0') return REMEDY_ERR_INVALID_ARGUMENT;

    if (config->arguments && config->arguments[0] != '\0') return REMEDY_ERR_NOT_SUPPORTED;
    if (config->channel_nonce && config->channel_nonce[0] != '\0') return REMEDY_ERR_NOT_SUPPORTED;
    if (config->timeout_ms != 0) return REMEDY_ERR_NOT_SUPPORTED;

    if (!is_valid_absolute_path(config->executable_path)) return REMEDY_ERR_INVALID_ARGUMENT;

    std::wstring wExecPath;
    std::wstring wWorkDir;
    const wchar_t* pWorkDir = NULL;

    try {
        if (!convert_utf8_to_wide(config->executable_path, wExecPath)) {
            return REMEDY_ERR_INVALID_ARGUMENT;
        }

        if (config->working_directory && config->working_directory[0] != '\0') {
            if (!convert_utf8_to_wide(config->working_directory, wWorkDir)) {
                return REMEDY_ERR_INVALID_ARGUMENT;
            }
            pWorkDir = wWorkDir.c_str();
        }
    } catch (const std::bad_alloc&) {
        return REMEDY_ERR_OUT_OF_MEMORY;
    }

    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = { 0 };

    BOOL procSuccess = CreateProcessW(
        wExecPath.c_str(),
        NULL,
        NULL, NULL, FALSE,
        CREATE_SUSPENDED,
        NULL, pWorkDir, &si, &pi
    );

    if (!procSuccess) return REMEDY_ERR_IPC_FAILURE;

#ifdef REMEDY_TEST_STAGED_FAILURE_SEAM
    if (g_test_fail_stage >= 1 && g_test_fail_stage <= 7) {
        BOOL dupOk = DuplicateHandle(
            GetCurrentProcess(), pi.hProcess,
            GetCurrentProcess(), &g_test_observer_handle,
            0, FALSE, DUPLICATE_SAME_ACCESS
        );
        if (!dupOk) {
            HANDLE hJob = NULL;
            do_verified_failure_cleanup(pi.hProcess, pi.hThread, hJob, false);
            return REMEDY_ERR_CONTAINMENT_FAILED;
        }
    }
    if (g_test_fail_stage == 1) {
        HANDLE hJob = NULL;
        do_verified_failure_cleanup(pi.hProcess, pi.hThread, hJob, false);
        return REMEDY_ERR_CONTAINMENT_FAILED;
    }
#endif

    HANDLE hJob = CreateJobObjectW(NULL, NULL);
    if (!hJob) {
        do_verified_failure_cleanup(pi.hProcess, pi.hThread, hJob, false);
        return REMEDY_ERR_CONTAINMENT_FAILED;
    }

#ifdef REMEDY_TEST_STAGED_FAILURE_SEAM
    if (g_test_fail_stage == 2) {
        do_verified_failure_cleanup(pi.hProcess, pi.hThread, hJob, false);
        return REMEDY_ERR_CONTAINMENT_FAILED;
    }
#endif

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli = { 0 };
    jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(hJob, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli))) {
        do_verified_failure_cleanup(pi.hProcess, pi.hThread, hJob, false);
        return REMEDY_ERR_CONTAINMENT_FAILED;
    }

#ifdef REMEDY_TEST_STAGED_FAILURE_SEAM
    if (g_test_fail_stage == 3) {
        do_verified_failure_cleanup(pi.hProcess, pi.hThread, hJob, false);
        return REMEDY_ERR_CONTAINMENT_FAILED;
    }
#endif

    if (!AssignProcessToJobObject(hJob, pi.hProcess)) {
        do_verified_failure_cleanup(pi.hProcess, pi.hThread, hJob, false);
        return REMEDY_ERR_CONTAINMENT_FAILED;
    }

#ifdef REMEDY_TEST_STAGED_FAILURE_SEAM
    if (g_test_fail_stage == 4) {
        do_verified_failure_cleanup(pi.hProcess, pi.hThread, hJob, true);
        return REMEDY_ERR_CONTAINMENT_FAILED;
    }
#endif

    if (ResumeThread(pi.hThread) == (DWORD)-1) {
        do_verified_failure_cleanup(pi.hProcess, pi.hThread, hJob, true);
        return REMEDY_ERR_CONTAINMENT_FAILED;
    }

#ifdef REMEDY_TEST_STAGED_FAILURE_SEAM
    if (g_test_fail_stage == 5) {
        do_verified_failure_cleanup(pi.hProcess, pi.hThread, hJob, true);
        return REMEDY_ERR_CONTAINMENT_FAILED;
    }
#endif

    win32_worker_entry* entry = new (std::nothrow) win32_worker_entry();
    if (!entry) {
        do_verified_failure_cleanup(pi.hProcess, pi.hThread, hJob, true);
        return REMEDY_ERR_OUT_OF_MEMORY;
    }

#ifdef REMEDY_TEST_STAGED_FAILURE_SEAM
    if (g_test_fail_stage == 6) {
        delete entry;
        do_verified_failure_cleanup(pi.hProcess, pi.hThread, hJob, true);
        return REMEDY_ERR_OUT_OF_MEMORY;
    }
#endif

    entry->process_handle = pi.hProcess;
    entry->thread_handle = pi.hThread;
    entry->job_handle = hJob;
    entry->process_id = pi.dwProcessId;

    remedy_worker_token_t token = REMEDY_INVALID_WORKER_TOKEN;
    try {
        std::lock_guard<std::mutex> lock(g_worker_mutex);
        token = generate_unique_token_locked();
        if (token == REMEDY_INVALID_WORKER_TOKEN) {
            delete entry;
            do_verified_failure_cleanup(pi.hProcess, pi.hThread, hJob, true);
            return REMEDY_ERR_OUT_OF_MEMORY;
        }
        g_worker_table[token] = entry;
    } catch (const std::bad_alloc&) {
        delete entry;
        do_verified_failure_cleanup(pi.hProcess, pi.hThread, hJob, true);
        return REMEDY_ERR_OUT_OF_MEMORY;
    }

#ifdef REMEDY_TEST_STAGED_FAILURE_SEAM
    if (g_test_fail_stage == 7) {
        {
            std::lock_guard<std::mutex> lock(g_worker_mutex);
            g_worker_table.erase(token);
        }
        do_verified_failure_cleanup(entry->process_handle, entry->thread_handle, entry->job_handle, true);
        if (entry->process_handle != NULL || entry->thread_handle != NULL || entry->job_handle != NULL) {
            remedy_fail_stop_containment_breach(entry->process_handle, entry->thread_handle, entry->job_handle);
        }
        delete entry;
        return REMEDY_ERR_CONTAINMENT_FAILED;
    }
#endif

    *out_token = token;
    return REMEDY_OK;
}

remedy_err_t worker_port_request_quiescence(remedy_worker_token_t token) {
    return REMEDY_ERR_NOT_SUPPORTED;
}

remedy_err_t worker_port_terminate(remedy_worker_token_t token) {
    if (token == REMEDY_INVALID_WORKER_TOKEN) return REMEDY_ERR_INVALID_ARGUMENT;

    win32_worker_entry* entry = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_worker_mutex);
        auto it = g_worker_table.find(token);
        if (it == g_worker_table.end()) return REMEDY_ERR_INVALID_ARGUMENT;
        entry = it->second;
    }

    if (entry->job_handle != NULL) {
        if (CloseHandle(entry->job_handle)) {
            entry->job_handle = NULL;
        } else {
            return REMEDY_ERR_CONTAINMENT_FAILED;
        }
    }
    return REMEDY_OK;
}

remedy_err_t worker_port_wait_for_death(remedy_worker_token_t token, uint32_t timeout_ms, bool* out_died) {
    if (!out_died) return REMEDY_ERR_INVALID_ARGUMENT;
    *out_died = false;

    if (token == REMEDY_INVALID_WORKER_TOKEN) return REMEDY_ERR_INVALID_ARGUMENT;

    win32_worker_entry* entry = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_worker_mutex);
        auto it = g_worker_table.find(token);
        if (it == g_worker_table.end()) return REMEDY_ERR_INVALID_ARGUMENT;
        entry = it->second;
    }

    if (!entry->process_handle) return REMEDY_ERR_INVALID_ARGUMENT;

    DWORD res = WaitForSingleObject(entry->process_handle, timeout_ms);
    if (res == WAIT_OBJECT_0) {
        *out_died = true;
        return REMEDY_OK;
    } else if (res == WAIT_TIMEOUT) {
        *out_died = false;
        return REMEDY_ERR_TIMEOUT;
    } else {
        *out_died = false;
        return REMEDY_ERR_WAIT_FAILED;
    }
}

remedy_err_t worker_port_destroy(remedy_worker_token_t token) {
    if (token == REMEDY_INVALID_WORKER_TOKEN) return REMEDY_ERR_INVALID_ARGUMENT;

    std::lock_guard<std::mutex> lock(g_worker_mutex);
    auto it = g_worker_table.find(token);
    if (it == g_worker_table.end()) return REMEDY_ERR_INVALID_ARGUMENT;

    win32_worker_entry* entry = it->second;

    if (entry->job_handle != NULL) return REMEDY_ERR_INVALID_ARGUMENT;
    if (WaitForSingleObject(entry->process_handle, 0) != WAIT_OBJECT_0) return REMEDY_ERR_INVALID_ARGUMENT;

    if (entry->thread_handle != NULL) {
        if (CloseHandle(entry->thread_handle)) {
            entry->thread_handle = NULL;
        } else {
            return REMEDY_ERR_CONTAINMENT_FAILED;
        }
    }

    if (entry->process_handle != NULL) {
        if (CloseHandle(entry->process_handle)) {
            entry->process_handle = NULL;
        } else {
            return REMEDY_ERR_CONTAINMENT_FAILED;
        }
    }

    if (entry->job_handle == NULL && entry->thread_handle == NULL && entry->process_handle == NULL) {
        g_worker_table.erase(it);
        delete entry;
        return REMEDY_OK;
    }

    return REMEDY_ERR_CONTAINMENT_FAILED;
}

} // extern "C"
