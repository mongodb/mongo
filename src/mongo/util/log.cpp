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
#include "mongo/util/concurrency/thread_name.h"
#include "mongo/util/concurrency/threadlocal.h"
#include "mongo/util/stacktrace.h"
#include "mongo/util/text.h"
#include "mongo/util/time_support.h"

using namespace std;

// TODO: Win32 unicode console writing (in logger/console_appender?).
// TODO: Extra log context appending, and re-enable log_user_*.js
// TODO: Eliminate cout/cerr.

namespace mongo {

namespace {
const char kUnknownMsg[] = "Unknown error";
const int kBuflen = 256;  // strerror strings in non-English locales can be large.
}

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
         it != result.end();
         it++) {
        warning() << "Rotating log file " << it->first << " failed: " << it->second.toString()
                  << endl;
    }
    return result.empty();
}

string errnoWithDescription(int errNumber) {
#if defined(_WIN32)
    if (errNumber < 0)
        errNumber = GetLastError();
#else
    if (errNumber < 0)
        errNumber = errno;
#endif

    char buf[kBuflen];
    char* msg{nullptr};

#if defined(__GNUC__) && defined(_GNU_SOURCE)
    msg = strerror_r(errNumber, buf, kBuflen);
#elif defined(_WIN32)

    LPWSTR errorText = nullptr;
    FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_ALLOCATE_BUFFER |
                       FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr,
                   errNumber,
                   0,
                   reinterpret_cast<LPWSTR>(&errorText),  // output
                   0,                                     // minimum size for output buffer
                   nullptr);

    if (errorText) {
        string utf8ErrorText = toUtf8String(errorText);
        auto size = utf8ErrorText.find_first_of("\r\n");
        if (size == string::npos) {  // not found
            size = utf8ErrorText.length();
        }

        if (size >= kBuflen) {
            size = kBuflen - 1;
        }

        memcpy(buf, utf8ErrorText.c_str(), size);
        buf[size] = '\0';
        msg = buf;
        LocalFree(errorText);
    } else if (strerror_s(buf, kBuflen, errNumber) != 0) {
        msg = buf;
    }
#else /* XSI strerror_r */
    if (strerror_r(errNumber, buf, kBuflen) == 0) {
        msg = buf;
    }
#endif

    if (!msg) {
        return {kUnknownMsg};
    }

    return {msg};
}

std::pair<int, std::string> errnoAndDescription() {
#if defined(_WIN32)
    int errNumber = GetLastError();
#else
    int errNumber = errno;
#endif
    return {errNumber, errnoWithDescription(errNumber)};
}

void logContext(const char* errmsg) {
    if (errmsg) {
        log() << errmsg << endl;
    }
    // NOTE: We disable long-line truncation for the stack trace, because the JSON representation of
    // the stack trace can sometimes exceed the long line limit.
    printStackTrace(log().setIsTruncatable(false).stream());
}

Tee* const warnings = RamLog::get("warnings");  // Things put here go in serverStatus
Tee* const startupWarningsLog = RamLog::get("startupWarnings");  // intentionally leaked

}  // namespace mongo
