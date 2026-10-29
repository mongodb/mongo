// Native entry point for the PowerShell test supervisor. CreateProcessW starts this directly,
// preserving arguments containing %NAME%, quotes, and empty strings without cmd.exe expansion.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "bazel/windows_test_launcher/windows_test_launcher.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include <windows.h>

namespace {
using mongo::windows_test_launcher::quoteArgument;

int reportError(const char* operation) {
    std::fprintf(
        stderr, "windows_test_launcher: %s failed: Win32 error %lu\n", operation, GetLastError());
    return 2;
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "windows_test_launcher: expected the test binary\n");
        return 2;
    }
    // Paths can exceed MAX_PATH even though Bazel uses a short launcher filename.
    std::vector<wchar_t> path(32768);
    DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length == path.size()) {
        return reportError("GetModuleFileNameW");
    }
    std::wstring script(path.data(), length);
    script = script.substr(0, script.find_last_of(L"/\\") + 1) + L"wtl.ps1";

    wchar_t temporaryDirectory[MAX_PATH + 1]{};
    length = GetTempPathW(MAX_PATH + 1, temporaryDirectory);
    if (!length || length > MAX_PATH) {
        return reportError("GetTempPathW");
    }
    wchar_t payloadPath[MAX_PATH + 1]{};
    if (!GetTempFileNameW(temporaryDirectory, L"wtl", 0, payloadPath)) {
        return reportError("GetTempFileNameW");
    }
    // A NUL-delimited UTF-16 payload avoids PowerShell 5.1's native argument parsing. Arguments
    // cannot contain NUL, and the final delimiter lets PowerShell preserve a trailing empty arg.
    std::wstring arguments;
    for (int i = 1; i < argc; ++i) {
        arguments.append(argv[i]);
        arguments += L'\0';
    }
    std::ofstream payload(std::filesystem::path(payloadPath), std::ios::binary | std::ios::trunc);
    payload.write(reinterpret_cast<const char*>(arguments.data()),
                  static_cast<std::streamsize>(arguments.size() * sizeof(wchar_t)));
    // Flush and close before PowerShell reads the file, including checking for close errors.
    payload.close();
    bool ok = static_cast<bool>(payload);
    int status = 0;
    if (!ok) {
        std::fprintf(stderr, "windows_test_launcher: failed to write argument payload\n");
        status = 2;
    }
    if (ok) {
        std::wstring command =
            L"powershell.exe -NoLogo -NoProfile -NonInteractive "
            L"-ExecutionPolicy Bypass -File " +
            quoteArgument(script) + L" " + quoteArgument(payloadPath);
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(nullptr,
                            command.data(),
                            nullptr,
                            nullptr,
                            TRUE,
                            0,
                            nullptr,
                            nullptr,
                            &startup,
                            &process)) {
            status = reportError("CreateProcessW");
        } else {
            CloseHandle(process.hThread);
            if (WaitForSingleObject(process.hProcess, INFINITE) != WAIT_OBJECT_0) {
                status = reportError("WaitForSingleObject");
            } else {
                DWORD exitCode;
                status = GetExitCodeProcess(process.hProcess, &exitCode)
                    ? static_cast<int>(exitCode)
                    : reportError("GetExitCodeProcess");
            }
            CloseHandle(process.hProcess);
        }
    }
    // PowerShell normally deletes the payload after reading it. Also clean up launch failures.
    std::error_code removeError;
    std::filesystem::remove(std::filesystem::path(payloadPath), removeError);
    return status;
}
