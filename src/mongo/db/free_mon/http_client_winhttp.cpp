/**
 * Copyright (C) 2018 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#ifndef _WIN32
#error This file should only be built on Windows
#endif
#ifndef _UNICODE
#error This file assumes a UNICODE WIN32 build
#endif

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kNetwork

#include "mongo/platform/basic.h"

#include <string>
#include <vector>
#include <versionhelpers.h>
#include <winhttp.h>

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/free_mon/free_mon_http.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/errno_util.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/text.h"
#include "mongo/util/winutil.h"

namespace mongo {
namespace {

const DWORD kResolveTimeout = 60 * 1000;
const DWORD kConnectTimeout = 60 * 1000;
const DWORD kSendTimeout = 120 * 1000;
const DWORD kReceiveTimeout = 120 * 1000;

const LPCWSTR kAcceptTypes[] = {
    L"application/octet-stream", nullptr,
};

struct ProcessedUrl {
    bool https;
    INTERNET_PORT port;
    std::wstring username;
    std::wstring password;
    std::wstring hostname;
    std::wstring path;
    std::wstring query;
};

StatusWith<ProcessedUrl> parseUrl(const std::wstring& url) {
    URL_COMPONENTS comp;
    ZeroMemory(&comp, sizeof(comp));
    comp.dwStructSize = sizeof(comp);

    // Request user, password, host, port, path, and extra(query).
    comp.dwUserNameLength = 1;
    comp.dwPasswordLength = 1;
    comp.dwHostNameLength = 1;
    comp.dwUrlPathLength = 1;
    comp.dwExtraInfoLength = 1;

    if (!WinHttpCrackUrl(url.c_str(), url.size(), 0, &comp)) {
        return {ErrorCodes::BadValue, "Unable to parse URL"};
    }

    ProcessedUrl ret;
    ret.https = (comp.nScheme == INTERNET_SCHEME_HTTPS);

    if (comp.lpszUserName) {
        ret.username = std::wstring(comp.lpszUserName, comp.dwUserNameLength);
    }

    if (comp.lpszPassword) {
        ret.password = std::wstring(comp.lpszPassword, comp.dwPasswordLength);
    }

    if (comp.lpszHostName) {
        ret.hostname = std::wstring(comp.lpszHostName, comp.dwHostNameLength);
    }

    if (comp.nPort) {
        ret.port = comp.nPort;
    } else if (ret.https) {
        ret.port = INTERNET_DEFAULT_HTTPS_PORT;
    } else {
        ret.port = INTERNET_DEFAULT_HTTP_PORT;
    }

    if (comp.lpszUrlPath) {
        ret.path = std::wstring(comp.lpszUrlPath, comp.dwUrlPathLength);
    }

    if (comp.lpszExtraInfo) {
        ret.query = std::wstring(comp.lpszExtraInfo, comp.dwExtraInfoLength);
    }

    return ret;
}

class FreeMonWinHttpClient : public FreeMonHttpClientInterface {
public:
    explicit FreeMonWinHttpClient(std::unique_ptr<executor::ThreadPoolTaskExecutor> executor)
        : _executor(std::move(executor)) {}
    ~FreeMonWinHttpClient() final = default;

    Future<std::vector<uint8_t>> postAsync(StringData url, const BSONObj obj) final {
        auto urlString = url.toString();

        auto pf = makePromiseFuture<std::vector<uint8_t>>();
        uassertStatusOK(
            _executor->scheduleWork([ shared_promise = pf.promise.share(), urlString, obj ](
                const executor::TaskExecutor::CallbackArgs& cbArgs) mutable {
                doPost(shared_promise, urlString, obj);
            }));

        return std::move(pf.future);
    }

private:
    static void doPost(SharedPromise<std::vector<uint8_t>> shared_promise,
                       const std::string& urlString,
                       const BSONObj& obj) try {
        const auto setError = [&shared_promise](StringData reason) {
            const auto msg = errnoWithDescription(GetLastError());
            shared_promise.setError(
                {ErrorCodes::OperationFailed, str::stream() << reason << ": " << msg});
        };

        // Break down URL for handling below.
        auto swUrl = parseUrl(toNativeString(urlString.c_str()));
        if (!swUrl.isOK()) {
            shared_promise.setError(swUrl.getStatus());
            return;
        }
        auto url = std::move(swUrl.getValue());

        if (!url.https && !getTestCommandsEnabled()) {
            shared_promise.setError(
                {ErrorCodes::BadValue, "Free Monitoring endpoint must be https://"});
            return;
        }
        if (!url.username.empty() || !url.password.empty()) {
            shared_promise.setError(
                {ErrorCodes::BadValue, "Free Monitoring endpoint must not use authentication"});
            return;
        }

        // Cleanup handled in a guard rather than UniquePtrs to ensure order.
        HINTERNET session = nullptr, connect = nullptr, request = nullptr;
        auto guard = MakeGuard([&] {
            if (request) {
                WinHttpCloseHandle(request);
            }
            if (connect) {
                WinHttpCloseHandle(connect);
            }
            if (session) {
                WinHttpCloseHandle(session);
            }
        });

        DWORD accessType = WINHTTP_ACCESS_TYPE_DEFAULT_PROXY;
        if (IsWindows8Point1OrGreater()) {
            accessType = WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY;
        }

        session = WinHttpOpen(L"MongoDB Free Monitoring Client/Windows",
                              accessType,
                              WINHTTP_NO_PROXY_NAME,
                              WINHTTP_NO_PROXY_BYPASS,
                              0);
        if (!session) {
            setError("Failed creating an HTTP session");
            return;
        }

        if (!WinHttpSetTimeouts(
                session, kResolveTimeout, kConnectTimeout, kSendTimeout, kReceiveTimeout)) {
            setError("Failed setting HTTP timeout");
            return;
        }

        connect = WinHttpConnect(session, url.hostname.c_str(), url.port, 0);
        if (!connect) {
            setError("Failed connecting to remote host");
            return;
        }

        request = WinHttpOpenRequest(connect,
                                     L"POST",
                                     (url.path + url.query).c_str(),
                                     nullptr,
                                     WINHTTP_NO_REFERER,
                                     const_cast<LPCWSTR*>(kAcceptTypes),
                                     url.https ? WINHTTP_FLAG_SECURE : 0);
        if (!request) {
            setError("Failed initializing HTTP request");
            return;
        }

        if (!url.username.empty() || !url.password.empty()) {
            if (!WinHttpSetCredentials(request,
                                       WINHTTP_AUTH_TARGET_SERVER,
                                       WINHTTP_AUTH_SCHEME_DIGEST,
                                       url.username.c_str(),
                                       url.password.c_str(),
                                       0)) {
                setError("Failed setting authentication credentials");
                return;
            }
        }

        if (!WinHttpSendRequest(request,
                                L"Content-type: application/octet-stream\r\n",
                                -1L,
                                const_cast<void*>(static_cast<const void*>(obj.objdata())),
                                obj.objsize(),
                                obj.objsize(),
                                0)) {
            setError("Failed sending HTTP request");
            return;
        }

        if (!WinHttpReceiveResponse(request, nullptr)) {
            setError("Failed receiving response from server");
            return;
        }

        DWORD statusCode = 0;
        DWORD statusCodeLength = sizeof(statusCode);

        if (!WinHttpQueryHeaders(request,
                                 WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX,
                                 &statusCode,
                                 &statusCodeLength,
                                 WINHTTP_NO_HEADER_INDEX)) {
            setError("Error querying status from server");
            return;
        }

        if (statusCode != 200) {
            shared_promise.setError(
                Status(ErrorCodes::OperationFailed,
                       str::stream() << "Unexpected http status code from server: " << statusCode));
            return;
        }

        // Marshal response into vector.
        std::vector<uint8_t> ret;
        auto sz = ret.size();
        for (;;) {
            DWORD len = 0;
            if (!WinHttpQueryDataAvailable(request, &len)) {
                setError("Failed receiving response data");
                return;
            }
            if (!len) {
                break;
            }
            ret.resize(sz + len);
            if (!WinHttpReadData(request, ret.data() + sz, len, &len)) {
                setError("Failed reading response data");
                return;
            }
            sz += len;
        }
        ret.resize(sz);

        shared_promise.emplaceValue(std::move(ret));
    } catch (...) {
        shared_promise.setError(exceptionToStatus());
    }

private:
    std::unique_ptr<executor::ThreadPoolTaskExecutor> _executor;
};

}  // namespace
}  // namespace mongo

std::unique_ptr<mongo::FreeMonHttpClientInterface> mongo::createFreeMonHttpClient(
    std::unique_ptr<executor::ThreadPoolTaskExecutor> executor) {
    return std::make_unique<FreeMonWinHttpClient>(std::move(executor));
}
