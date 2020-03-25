/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
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
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/errno_util.h"
#include "mongo/util/net/http_client.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"
#include "mongo/util/text.h"
#include "mongo/util/winutil.h"

namespace mongo {
namespace {

const LPCWSTR kAcceptTypes[] = {
    L"application/octet-stream",
    nullptr,
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

class WinHttpClient : public HttpClient {
public:
    ~WinHttpClient() final = default;

    void allowInsecureHTTP(bool allow) final {
        _allowInsecureHTTP = allow;
    }

    void setHeaders(const std::vector<std::string>& headers) final {
        // 1. Concatenate all headers with \r\n line endings.
        StringBuilder sb;
        for (const auto& header : headers) {
            sb << header << "\r\n";
        }
        auto header = sb.str();

        // 2. Remove final \r\n delimiter.
        if (header.size() >= 2) {
            header.pop_back();
            header.pop_back();
        }

        // 3. Expand to Windows Unicode wide string.
        _headers = toNativeString(header.c_str());
    }

    void setConnectTimeout(Seconds timeout) final {
        _connectTimeout = timeout;
    }

    void setTimeout(Seconds timeout) final {
        _timeout = timeout;
    }

    HttpReply request(HttpMethod methodType, StringData urlSD, ConstDataRange cdrData) const final {
        LPCWSTR method = L"GET";
        LPVOID data = const_cast<void*>(static_cast<const void*>(cdrData.data()));
        DWORD data_len = cdrData.length();
        switch (methodType) {
            case HttpMethod::kGET:
                uassert(ErrorCodes::BadValue,
                        "GET requests do not support content",
                        cdrData.length() == 0);
                break;
            case HttpMethod::kPOST:
                method = L"POST";
                break;
            case HttpMethod::kPUT:
                method = L"PUT";
                break;
            default:
                MONGO_UNREACHABLE;
        }

        const auto uassertWithErrno = [](StringData reason, bool ok) {
            const auto msg = errnoWithDescription(GetLastError());
            uassert(ErrorCodes::OperationFailed, str::stream() << reason << ": " << msg, ok);
        };

        // Break down URL for handling below.
        const auto urlString = toNativeString(urlSD.toString().c_str());
        auto url = uassertStatusOK(parseUrl(urlString));
        uassert(
            ErrorCodes::BadValue, "URL endpoint must be https://", url.https || _allowInsecureHTTP);

        // Cleanup handled in a guard rather than UniquePtrs to ensure order.
        HINTERNET session = nullptr, connect = nullptr, request = nullptr;
        auto guard = makeGuard([&] {
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

        session = WinHttpOpen(L"MongoDB HTTP Client/Windows",
                              accessType,
                              WINHTTP_NO_PROXY_NAME,
                              WINHTTP_NO_PROXY_BYPASS,
                              0);
        uassertWithErrno("Failed creating an HTTP session", session);

        DWORD setting;
        DWORD settingLength = sizeof(setting);
        setting = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        uassertWithErrno(
            "Failed setting HTTP session option",
            WinHttpSetOption(session, WINHTTP_OPTION_REDIRECT_POLICY, &setting, settingLength));

        DWORD connectTimeout = durationCount<Milliseconds>(_connectTimeout);
        DWORD totalTimeout = durationCount<Milliseconds>(_timeout);
        uassertWithErrno("Failed setting HTTP timeout",
                         WinHttpSetTimeouts(
                             session, connectTimeout, connectTimeout, totalTimeout, totalTimeout));

        connect = WinHttpConnect(session, url.hostname.c_str(), url.port, 0);
        uassertWithErrno("Failed connecting to remote host", connect);

        request = WinHttpOpenRequest(connect,
                                     method,
                                     (url.path + url.query).c_str(),
                                     nullptr,
                                     WINHTTP_NO_REFERER,
                                     const_cast<LPCWSTR*>(kAcceptTypes),
                                     url.https ? WINHTTP_FLAG_SECURE : 0);
        uassertWithErrno("Failed initializing HTTP request", request);

        if (!url.username.empty() || !url.password.empty()) {
            auto result = WinHttpSetCredentials(request,
                                                WINHTTP_AUTH_TARGET_SERVER,
                                                WINHTTP_AUTH_SCHEME_DIGEST,
                                                url.username.c_str(),
                                                url.password.c_str(),
                                                0);
            uassertWithErrno("Failed setting authentication credentials", result);
        }

        uassertWithErrno(
            "Failed sending HTTP request",
            WinHttpSendRequest(request, _headers.c_str(), -1L, data, data_len, data_len, 0));

        if (!WinHttpReceiveResponse(request, nullptr)) {
            // Carve out timeout which doesn't translate well.
            const auto err = GetLastError();
            if (err == ERROR_WINHTTP_TIMEOUT) {
                uasserted(ErrorCodes::OperationFailed, "Timeout was reached");
            }
            const auto msg = errnoWithDescription(err);
            uasserted(ErrorCodes::OperationFailed,
                      str::stream() << "Failed receiving response from server"
                                    << ": " << msg);
        }

        DWORD statusCode = 0;
        DWORD statusCodeLength = sizeof(statusCode);

        uassertWithErrno("Error querying status from server",
                         WinHttpQueryHeaders(request,
                                             WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                             WINHTTP_HEADER_NAME_BY_INDEX,
                                             &statusCode,
                                             &statusCodeLength,
                                             WINHTTP_NO_HEADER_INDEX));

        DWORD len = 0;
        std::vector<char> buffer;
        DataBuilder ret(4096);
        for (;;) {
            len = 0;
            uassertWithErrno("Failed receiving response data",
                             WinHttpQueryDataAvailable(request, &len));
            if (!len) {
                break;
            }

            buffer.resize(len);
            uassertWithErrno("Failed reading response data",
                             WinHttpReadData(request, buffer.data(), len, &len));

            ConstDataRange cdr(buffer.data(), len);
            ret.writeAndAdvance(cdr);
        }

        DataBuilder headers(4096);
        if (!WinHttpQueryHeaders(request,
                                 WINHTTP_QUERY_RAW_HEADERS_CRLF,
                                 WINHTTP_HEADER_NAME_BY_INDEX,
                                 WINHTTP_NO_OUTPUT_BUFFER,
                                 &len,
                                 WINHTTP_NO_HEADER_INDEX) &&
            (GetLastError() == ERROR_INSUFFICIENT_BUFFER)) {
            buffer.resize(len);
            uassertWithErrno("Error querying headers from server",
                             WinHttpQueryHeaders(request,
                                                 WINHTTP_QUERY_RAW_HEADERS_CRLF,
                                                 WINHTTP_HEADER_NAME_BY_INDEX,
                                                 &buffer[0],
                                                 &len,
                                                 WINHTTP_NO_HEADER_INDEX));
            headers.writeAndAdvance(ConstDataRange(buffer.data(), len));
        }

        return HttpReply(statusCode, std::move(headers), std::move(ret));
    }

private:
    bool _allowInsecureHTTP = false;
    std::wstring _headers;
    Seconds _connectTimeout = kConnectionTimeout;
    Seconds _timeout = kTotalRequestTimeout;
};

}  // namespace

std::unique_ptr<HttpClient> HttpClient::create() {
    return std::make_unique<WinHttpClient>();
}

BSONObj HttpClient::getServerStatus() {
    return BSON("type"
                << "winhttp");
}

}  // namespace mongo
