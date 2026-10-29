// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#ifdef _WIN32


#pragma warning(push)
// C4091: 'typedef ': ignored on left of '' when no variable is declared
#pragma warning(disable : 4091)
#include <DbgHelp.h>
#pragma warning(pop)

#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/exit_code.h"
#include "mongo/util/quick_exit.h"
#include "mongo/util/stacktrace_windows.h"
#include "mongo/util/text.h"  // IWYU pragma: keep

#include <ostream>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl

namespace mongo {

namespace {
Atomic<bool> win32MinidumpEnabled{true};
}  // namespace

void setWin32MinidumpEnabled(bool enabled) {
    win32MinidumpEnabled.store(enabled);
}

static HMODULE hDbgHelp{};

namespace {
/* create a process dump.
    To use, load up windbg.  Set your symbol and source path.
    Open the crash dump file.  To see the crashing context, use .ecxr in windbg
    Consider using WER local dumps in the future
    */
void doMinidumpWithException(struct _EXCEPTION_POINTERS* exceptionInfo) {

    // Writing to the minidump may require loading symbols from additional libraries. There is a
    // risk that this process can lead to DLL "loader lock" where the process will not terminate
    // immediately upon failing to write the minidump to file. For automated testing that
    // intentionally triggers server termination, this fail point can prevent the process from
    // hanging by bypassing the normal dump creation.
    if (MONGO_unlikely(!win32MinidumpEnabled.load())) {
        LOGV2(12230000, "*** minidump creation is disabled");
        return;
    }

    WCHAR moduleFileName[MAX_PATH];

    DWORD ret = GetModuleFileNameW(nullptr, &moduleFileName[0], ARRAYSIZE(moduleFileName));
    if (ret == 0) {
        auto ec = lastSystemError();
        LOGV2(23130, "GetModuleFileName failed", "error"_attr = errorMessage(ec));

        // Fallback name
        wcscpy_s(moduleFileName, L"mongo");
    } else {
        WCHAR* dotStr = wcsrchr(&moduleFileName[0], L'.');
        if (dotStr != nullptr) {
            *dotStr = L'\0';
        }
    }

    std::wstring dumpName(moduleFileName);

    dumpName += L".";

    dumpName += toWideStringFromStringData(terseCurrentTimeForFilename());

    dumpName += L".mdmp";

    HANDLE hFile = CreateFileW(
        dumpName.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (INVALID_HANDLE_VALUE == hFile) {
        auto ec = lastSystemError();
        LOGV2(23131,
              "Failed to open minidump file",
              "dumpName"_attr = toUtf8String(dumpName.c_str()),
              "error"_attr = errorMessage(ec));
        return;
    }

    MINIDUMP_EXCEPTION_INFORMATION aMiniDumpInfo;
    aMiniDumpInfo.ThreadId = GetCurrentThreadId();
    aMiniDumpInfo.ExceptionPointers = exceptionInfo;
    aMiniDumpInfo.ClientPointers = FALSE;

    MINIDUMP_TYPE miniDumpType =
#ifdef MONGO_CONFIG_DEBUG_BUILD
        static_cast<MINIDUMP_TYPE>(MiniDumpWithFullMemory | MiniDumpIgnoreInaccessibleMemory);
#else
        static_cast<MINIDUMP_TYPE>(MiniDumpNormal | MiniDumpWithIndirectlyReferencedMemory |
                                   MiniDumpWithProcessThreadData | MiniDumpWithThreadInfo |
                                   MiniDumpWithUnloadedModules | MiniDumpWithTokenInformation);
#endif
    LOGV2(23132,
          "Writing minidump diagnostic file",
          "dumpName"_attr = toUtf8String(dumpName.c_str()));

    BOOL bstatus = MiniDumpWriteDump(GetCurrentProcess(),
                                     GetCurrentProcessId(),
                                     hFile,
                                     miniDumpType,
                                     exceptionInfo != nullptr ? &aMiniDumpInfo : nullptr,
                                     nullptr,
                                     nullptr);
    if (FALSE == bstatus) {
        auto ec = lastSystemError();
        LOGV2(23133, "Failed to create minidump", "error"_attr = errorMessage(ec));
    }

    CloseHandle(hFile);
}
}  // namespace

LONG WINAPI exceptionFilter(struct _EXCEPTION_POINTERS* excPointers) {
    char exceptionString[128];
    sprintf_s(exceptionString,
              sizeof(exceptionString),
              (excPointers->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION)
                  ? "(access violation)"
                  : "0x%08X",
              excPointers->ExceptionRecord->ExceptionCode);
    char addressString[32];
    sprintf_s(addressString,
              sizeof(addressString),
              "0x%p",
              excPointers->ExceptionRecord->ExceptionAddress);
    LOGV2_FATAL_CONTINUE(23134,
                         "Unhandled exception",
                         "exceptionString"_attr = exceptionString,
                         "addressString"_attr = addressString);
    if (excPointers->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        ULONG acType = excPointers->ExceptionRecord->ExceptionInformation[0];
        const char* acTypeString;
        switch (acType) {
            case 0:
                acTypeString = "read from";
                break;
            case 1:
                acTypeString = "write to";
                break;
            case 8:
                acTypeString = "DEP violation at";
                break;
            default:
                acTypeString = "unknown violation at";
                break;
        }
        sprintf_s(addressString,
                  sizeof(addressString),
                  " 0x%llx",
                  excPointers->ExceptionRecord->ExceptionInformation[1]);
        LOGV2_FATAL_CONTINUE(23135,
                             "Access violation",
                             "accessType"_attr = acTypeString,
                             "address"_attr = addressString);
    }

    LOGV2_FATAL_CONTINUE(23136, "*** stack trace for unhandled exception:");

    // Create a copy of context record because printWindowsStackTrace will mutate it.
    CONTEXT contextCopy(*(excPointers->ContextRecord));

    printWindowsStackTrace(contextCopy);

    doMinidumpWithException(excPointers);

    // Don't go through normal shutdown procedure. It may make things worse.
    // Do not go through _exit or ExitProcess(), terminate immediately
    LOGV2_FATAL_CONTINUE(23137, "*** immediate exit due to unhandled exception");
    TerminateProcess(GetCurrentProcess(), static_cast<UINT>(ExitCode::abrupt));

    // We won't reach here
    return EXCEPTION_EXECUTE_HANDLER;
}


LPTOP_LEVEL_EXCEPTION_FILTER filtLast = 0;

void setWindowsUnhandledExceptionFilter() {
    hDbgHelp = LoadLibraryA("DbgHelp");
    filtLast = SetUnhandledExceptionFilter(exceptionFilter);
}

Status onUpdateWin32MinidumpEnabled(bool newValue) {
    setWin32MinidumpEnabled(newValue);
    return Status::OK();
}

}  // namespace mongo

#else

#include "mongo/base/status.h"

namespace mongo {
void setWindowsUnhandledExceptionFilter() {}
void setWin32MinidumpEnabled(bool) {}
Status onUpdateWin32MinidumpEnabled(bool) {
    return Status::OK();
}
}  // namespace mongo

#endif  // _WIN32
