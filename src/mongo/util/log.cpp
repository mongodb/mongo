/*    Copyright 2009 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include "mongo/platform/basic.h"

#include "mongo/util/log.h"

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#include "mongo/logger/ramlog.h"
#include "mongo/logger/rotatable_file_manager.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/threadlocal.h"
#include "mongo/util/concurrency/thread_name.h"
#include "mongo/util/stacktrace.h"
#include "mongo/util/text.h"
#include "mongo/util/time_support.h"

using namespace std;

// TODO: Win32 unicode console writing (in logger/console_appender?).
// TODO: Extra log context appending, and re-enable log_user_*.js
// TODO: Eliminate cout/cerr.
// TODO: LogIndent (for mongodump).
// TODO: Eliminate rawOut.

namespace mongo {

    static logger::ExtraLogContextFn _appendExtraLogContext;

    Status logger::registerExtraLogContextFn(logger::ExtraLogContextFn contextFn) {
        if (!contextFn)
            return Status(ErrorCodes::BadValue, "Cannot register a NULL log context function.");
        if (_appendExtraLogContext) {
            return Status(ErrorCodes::AlreadyInitialized,
                          "Cannot call registerExtraLogContextFn multiple times.");
        }
        _appendExtraLogContext = contextFn;
        return Status::OK();
    }

    int tlogLevel = 0; // test log level. so we avoid overchattiness (somewhat) in the c++ unit tests

    const char *default_getcurns() { return ""; }
    const char * (*getcurns)() = default_getcurns;

    bool rotateLogs() {
        using logger::RotatableFileManager;
        RotatableFileManager* manager = logger::globalRotatableFileManager();
        RotatableFileManager::FileNameStatusPairVector result(
                manager->rotateAll("." + terseCurrentTime(false)));
        return result.empty();
    }

    string errnoWithDescription(int x) {
#if defined(_WIN32)
        if( x < 0 ) 
            x = GetLastError();
#else
        if( x < 0 ) 
            x = errno;
#endif
        stringstream s;
        s << "errno:" << x << ' ';

#if defined(_WIN32)
        LPWSTR errorText = NULL;
        FormatMessageW(
            FORMAT_MESSAGE_FROM_SYSTEM
            |FORMAT_MESSAGE_ALLOCATE_BUFFER
            |FORMAT_MESSAGE_IGNORE_INSERTS,
            NULL,
            x, 0,
            reinterpret_cast<LPWSTR>( &errorText ),  // output
            0, // minimum size for output buffer
            NULL);
        if( errorText ) {
            string x = toUtf8String(errorText);
            for( string::iterator i = x.begin(); i != x.end(); i++ ) {
                if( *i == '\n' || *i == '\r' )
                    break;
                s << *i;
            }
            LocalFree(errorText);
        }
        else
            s << strerror(x);
        /*
        DWORD n = FormatMessage(
            FORMAT_MESSAGE_ALLOCATE_BUFFER |
            FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
            NULL, x,
            MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
            (LPTSTR) &lpMsgBuf, 0, NULL);
        */
#else
        s << strerror(x);
#endif
        return s.str();
    }

    namespace {
        bool rawOutToStderr = false;
    } // namespace

    void setRawOutToStderr() {
        rawOutToStderr = true;
    }

    /*
     * NOTE(schwerin): Called from signal handlers; should not be taking locks or allocating
     * memory.
     */
    void rawOut(const StringData &s) {
        // Can't use STDxxx_FILENO macros since they don't exist on windows.
        const int fd = rawOutToStderr
                        ? 2 // STDERR_FILENO
                        : 1 // STDOUT_FILENO
                        ;

        const char* ptr = s.rawData();
        size_t bytesRemaining = s.size();
        while (bytesRemaining) {
#ifdef _WIN32
            int ret = _write(fd, ptr, bytesRemaining);
#else
            ssize_t ret = write(fd, ptr, bytesRemaining);
#endif
            if (ret < 0)
                return; // Nothing to do. Can't even log since that is what is failing.

            ptr += ret;
            bytesRemaining -= ret;
        }
    }

    void logContext(const char *errmsg) {
        if ( errmsg ) {
            problem() << errmsg << endl;
        }
        printStackTrace(problem().stream());
    }

    LogIndentLevel::LogIndentLevel() {
    }

    LogIndentLevel::~LogIndentLevel() {
    }

    Tee* const warnings = RamLog::get("warnings"); // Things put here go in serverStatus
    Tee* const startupWarningsLog = RamLog::get("startupWarnings");  // intentionally leaked

}  // namespace mongo
