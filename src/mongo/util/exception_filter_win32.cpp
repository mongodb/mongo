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

#ifdef _WIN32

#include <ostream>

#include "mongo/platform/basic.h"
#include <DbgHelp.h>
#include "mongo/util/assert_util.h"
#include "mongo/util/exit_code.h"
#include "mongo/util/log.h"
#include "mongo/util/stacktrace.h"
#include "mongo/util/text.h"

namespace mongo {

    /* create a process dump.
        To use, load up windbg.  Set your symbol and source path.
        Open the crash dump file.  To see the crashing context, use .ecxr
        */
    void doMinidump(struct _EXCEPTION_POINTERS* exceptionInfo) {
        LPCWSTR dumpFilename = L"mongo.dmp";
        HANDLE hFile = CreateFileW(dumpFilename,
            GENERIC_WRITE,
            0,
            NULL,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            NULL);
        if ( INVALID_HANDLE_VALUE == hFile ) {
            DWORD lasterr = GetLastError();
            log() << "failed to open minidump file " << toUtf8String(dumpFilename) << " : "
                  << errnoWithDescription( lasterr ) << std::endl;
            return;
        }

        MINIDUMP_EXCEPTION_INFORMATION aMiniDumpInfo;
        aMiniDumpInfo.ThreadId = GetCurrentThreadId();
        aMiniDumpInfo.ExceptionPointers = exceptionInfo;
        aMiniDumpInfo.ClientPointers = TRUE;

        log() << "writing minidump diagnostic file " << toUtf8String(dumpFilename) << std::endl;
        BOOL bstatus = MiniDumpWriteDump(GetCurrentProcess(),
            GetCurrentProcessId(),
            hFile,
            MiniDumpNormal,
            &aMiniDumpInfo,
            NULL,
            NULL);
        if ( FALSE == bstatus ) {
            DWORD lasterr = GetLastError();
            log() << "failed to create minidump : "
                  << errnoWithDescription( lasterr ) << std::endl;
        }

        CloseHandle(hFile);
    }

    LONG WINAPI exceptionFilter( struct _EXCEPTION_POINTERS *excPointers ) {
        char exceptionString[128];
        sprintf_s( exceptionString, sizeof( exceptionString ),
                ( excPointers->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION ) ?
                "(access violation)" : "0x%08X", excPointers->ExceptionRecord->ExceptionCode );
        char addressString[32];
        sprintf_s( addressString, sizeof( addressString ), "0x%p",
                 excPointers->ExceptionRecord->ExceptionAddress );
        log() << "*** unhandled exception " << exceptionString
              << " at " << addressString << ", terminating" << std::endl;
        if ( excPointers->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION ) {
            ULONG acType = excPointers->ExceptionRecord->ExceptionInformation[0];
            const char* acTypeString;
            switch ( acType ) {
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
            sprintf_s( addressString, sizeof( addressString ), " 0x%p",
                     excPointers->ExceptionRecord->ExceptionInformation[1] );
            log() << "*** access violation was a " << acTypeString << addressString << std::endl;
        }

        log() << "*** stack trace for unhandled exception:" << std::endl;
        printWindowsStackTrace( *excPointers->ContextRecord );
        doMinidump(excPointers);

        // Don't go through normal shutdown procedure. It may make things worse.
        log() << "*** immediate exit due to unhandled exception" << std::endl;
        ::_exit(EXIT_ABRUPT);

        // We won't reach here
        return EXCEPTION_EXECUTE_HANDLER;
    }

    LPTOP_LEVEL_EXCEPTION_FILTER filtLast = 0;

    void setWindowsUnhandledExceptionFilter() {
        filtLast = SetUnhandledExceptionFilter(exceptionFilter);
    }

} // namespace mongo

#else

namespace mongo {
    void setWindowsUnhandledExceptionFilter() { }
}

#endif // _WIN32
