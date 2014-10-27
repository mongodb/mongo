/*    Copyright 2009 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
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

    bool rotateLogs(bool renameFiles) {
        using logger::RotatableFileManager;
        RotatableFileManager* manager = logger::globalRotatableFileManager();
        RotatableFileManager::FileNameStatusPairVector result(
                manager->rotateAll(renameFiles, "." + terseCurrentTime(false)));
        for (RotatableFileManager::FileNameStatusPairVector::iterator it = result.begin();
                it != result.end(); it++) {
            warning() << "Rotating log file " << it->first << " failed: " << it->second.toString()
                    << endl;
        }
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

    void logContext(const char *errmsg) {
        if ( errmsg ) {
            log() << errmsg << endl;
        }
        printStackTrace(log().stream());
    }

    LogIndentLevel::LogIndentLevel() {
    }

    LogIndentLevel::~LogIndentLevel() {
    }

    Tee* const warnings = RamLog::get("warnings"); // Things put here go in serverStatus
    Tee* const startupWarningsLog = RamLog::get("startupWarnings");  // intentionally leaked

}  // namespace mongo
