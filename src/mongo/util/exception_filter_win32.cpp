/**
*    Copyright (C) 2012 10gen Inc.
*
*    This program is free software: you can redistribute it and/or modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects
*    for all of the code used other than as permitted herein. If you modify
*    file(s) with this exception, you may extend this exception to your
*    version of the file(s), but you are not obligated to do so. If you do not
*    wish to do so, delete this exception statement from your version. If you
*    delete this exception statement from all source files in the program,
*    then also delete it in the license file.
*/

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kControl

#ifdef _WIN32

#include "mongo/platform/basic.h"

#include <DbgHelp.h>
#include <ostream>

#include "mongo/config.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/exit_code.h"
#include "mongo/util/log.h"
#include "mongo/util/quick_exit.h"
#include "mongo/util/stacktrace.h"
#include "mongo/util/text.h"

namespace mongo {

namespace {
/* create a process dump.
    To use, load up windbg.  Set your symbol and source path.
    Open the crash dump file.  To see the crashing context, use .ecxr in windbg
    TODO: consider using WER local dumps in the future
    */
void doMinidumpWithException(struct _EXCEPTION_POINTERS* exceptionInfo) {
    WCHAR moduleFileName[MAX_PATH];

    DWORD ret = GetModuleFileNameW(NULL, &moduleFileName[0], ARRAYSIZE(moduleFileName));
    if (ret == 0) {
        int gle = GetLastError();
        log() << "GetModuleFileName failed " << errnoWithDescription(gle);

        // Fallback name
        wcscpy_s(moduleFileName, L"mongo");
    } else {
        WCHAR* dotStr = wcschr(&moduleFileName[0], L'.');
        if (dotStr != NULL) {
            *dotStr = L'\0';
        }
    }

    std::wstring dumpName(moduleFileName);

    std::string currentTime = terseCurrentTime(false);

    dumpName += L".";

    dumpName += toWideString(currentTime.c_str());

    dumpName += L".mdmp";

    HANDLE hFile = CreateFileW(
        dumpName.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (INVALID_HANDLE_VALUE == hFile) {
        DWORD lasterr = GetLastError();
        log() << "failed to open minidump file " << toUtf8String(dumpName.c_str()) << " : "
              << errnoWithDescription(lasterr) << std::endl;
        return;
    }

    MINIDUMP_EXCEPTION_INFORMATION aMiniDumpInfo;
    aMiniDumpInfo.ThreadId = GetCurrentThreadId();
    aMiniDumpInfo.ExceptionPointers = exceptionInfo;
    aMiniDumpInfo.ClientPointers = FALSE;

    MINIDUMP_TYPE miniDumpType =
#ifdef MONGO_CONFIG_DEBUG_BUILD
        MiniDumpWithFullMemory;
#else
        static_cast<MINIDUMP_TYPE>(MiniDumpNormal | MiniDumpWithIndirectlyReferencedMemory |
                                   MiniDumpWithProcessThreadData);
#endif
    log() << "writing minidump diagnostic file " << toUtf8String(dumpName.c_str()) << std::endl;

    BOOL bstatus = MiniDumpWriteDump(GetCurrentProcess(),
                                     GetCurrentProcessId(),
                                     hFile,
                                     miniDumpType,
                                     exceptionInfo != NULL ? &aMiniDumpInfo : NULL,
                                     NULL,
                                     NULL);
    if (FALSE == bstatus) {
        DWORD lasterr = GetLastError();
        log() << "failed to create minidump : " << errnoWithDescription(lasterr) << std::endl;
    }

    CloseHandle(hFile);
}

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
    log() << "*** unhandled exception " << exceptionString << " at " << addressString
          << ", terminating" << std::endl;
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
                  " 0x%p",
                  excPointers->ExceptionRecord->ExceptionInformation[1]);
        log() << "*** access violation was a " << acTypeString << addressString << std::endl;
    }

    log() << "*** stack trace for unhandled exception:" << std::endl;

    // Create a copy of context record because printWindowsStackTrace will mutate it.
    CONTEXT contextCopy(*(excPointers->ContextRecord));

    printWindowsStackTrace(contextCopy);

    doMinidumpWithException(excPointers);

    // Don't go through normal shutdown procedure. It may make things worse.
    // Do not go through _exit or ExitProcess(), terminate immediately
    log() << "*** immediate exit due to unhandled exception" << std::endl;
    TerminateProcess(GetCurrentProcess(), EXIT_ABRUPT);

    // We won't reach here
    return EXCEPTION_EXECUTE_HANDLER;
}
}

LPTOP_LEVEL_EXCEPTION_FILTER filtLast = 0;

void setWindowsUnhandledExceptionFilter() {
    filtLast = SetUnhandledExceptionFilter(exceptionFilter);
}

}  // namespace mongo

#else

namespace mongo {
void setWindowsUnhandledExceptionFilter() {}
}

#endif  // _WIN32
