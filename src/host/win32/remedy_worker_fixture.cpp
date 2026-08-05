#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wchar.h>

int main() {
    wchar_t cwd[1024] = { 0 };
    DWORD len = GetCurrentDirectoryW(1024, cwd);
    if (len == 0 || len >= 1024) {
        return 1;
    }

    wchar_t marker_path[1024] = { 0 };
    if (_snwprintf(marker_path, 1024, L"%s\\remedy_fixture_marker.tmp", cwd) < 0) {
        return 2;
    }

    HANDLE hFile = CreateFileW(
        marker_path,
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );

    if (hFile == INVALID_HANDLE_VALUE) {
        return 3;
    }

    const char marker_contents[] = "OK";
    DWORD bytes_written = 0;
    BOOL write_ok = WriteFile(hFile, marker_contents, (DWORD)sizeof(marker_contents) - 1, &bytes_written, NULL);
    if (!write_ok || bytes_written != sizeof(marker_contents) - 1) {
        BOOL close_ok = CloseHandle(hFile);
        if (!close_ok) {
            return 8;
        }
        return 4;
    }

    BOOL flush_ok = FlushFileBuffers(hFile);
    if (!flush_ok) {
        BOOL close_ok = CloseHandle(hFile);
        if (!close_ok) {
            return 9;
        }
        return 5;
    }

    BOOL close_ok = CloseHandle(hFile);
    if (!close_ok) {
        return 6;
    }

    // Genuinely non-settling Win32 wait until externally terminated
    DWORD wait_res = WaitForSingleObject(GetCurrentProcess(), INFINITE);
    if (wait_res != WAIT_OBJECT_0) {
        return 7;
    }

    return 0;
}
