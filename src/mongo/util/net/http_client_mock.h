/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#pragma once

#include "mongo/stdx/unordered_map.h"
#include "mongo/util/net/http_client.h"
#include <cstdint>

namespace mongo {

class MockHttpClient : public HttpClient {
public:
    MockHttpClient() = default;

    void allowInsecureHTTP(bool allow) final {
        _allow = allow;
    }

    void setHeaders(const std::vector<std::string>& headers) final {}

    HttpReply request(HttpMethod method,
                      StringData url,
                      ConstDataRange data = {nullptr, 0}) const final;

    struct Request {
        HttpMethod method;
        std::string url;

        template <typename H>
        friend H AbslHashValue(H h, const Request& request) {
            return H::combine(std::move(h), request.method, request.url);
        }
        friend bool operator==(const Request& my, const Request& other) {
            return my.method == other.method && my.url == other.url;
        }
    };
    struct Response {
        std::uint16_t code;
        std::vector<std::string> header;
        std::string body;
    };
    void expect(Request request, Response response) {
        _expectations.emplace(std::move(request), std::move(response));
    }

private:
    bool _allow = false;
    mutable stdx::unordered_map<Request, Response> _expectations;
};

}  // namespace mongo
