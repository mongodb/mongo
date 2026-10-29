// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/util/net/http_client_mock.h"

#include "mongo/util/assert_util.h"

#include <string_view>

namespace mongo {
using namespace std::literals::string_view_literals;


HttpClient::HttpReply MockHttpClient::request(HttpMethod method,
                                              std::string_view url,
                                              ConstDataRange data) const {
    if (url.starts_with("http://") && !url.starts_with("http://localhost")) {
        uassert(ErrorCodes::IllegalOperation,
                "Unsafe and unexpected HTTP operation performed with mock HttpClient",
                _allow);
    }

    auto it = _expectations.find(Request{method, std::string{url}});
    uassert(ErrorCodes::OperationFailed,
            "Unexpected request submitted to mock HttpClient",
            it != _expectations.end());

    auto reply = it->second;
    DataBuilder headerBuilder;
    for (std::string_view line : reply.header) {
        uassertStatusOK(headerBuilder.writeAndAdvance(line));
        uassertStatusOK(headerBuilder.writeAndAdvance("\n"sv));
    }
    DataBuilder bodyBuilder;
    uassertStatusOK(bodyBuilder.writeAndAdvance<std::string_view>(reply.body));

    HttpClient::HttpReply ret(reply.code, std::move(headerBuilder), std::move(bodyBuilder));

    _expectations.erase(it);

    return ret;
}

}  // namespace mongo
