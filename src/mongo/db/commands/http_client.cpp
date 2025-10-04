/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/util/net/http_client.h"

#include "mongo/base/data_builder.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/string_data.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/http_client_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/net/hostandport.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>

#include <boost/optional/optional.hpp>

namespace mongo {
namespace {

bool isLocalhostURI(StringData uri) {
    StringData host;

    if (uri.starts_with("http://")) {
        host = uri.substr(strlen("http://"));
    } else if (uri.starts_with("https://")) {
        host = uri.substr(strlen("https://"));
    } else {
        // Anything not http(s) is fail-closed to non-localhost.
        return false;
    }

    // Pessimistic URI parsing.
    // This is a test-only API,
    // and in our tests we expect well-formed URIs.
    // Anything else is considered suspect.
    auto end = host.find('/');
    if (end == std::string::npos) {
        return false;
    }

    host = host.substr(0, end);

    auto start = host.find('@');
    if (start != std::string::npos) {
        host = host.substr(start + 1);
    }

    HostAndPort hp(host);
    return hp.isLocalHost();
}

std::string stringFromDataBuilder(DataBuilder& builder) {
    const auto sz = builder.size();
    std::string ret;
    ret.resize(sz);
    std::copy_n(builder.release().get(), sz, &ret[0]);
    return ret;
}

class CmdHttpClient final : public TypedCommand<CmdHttpClient> {
public:
    using Request = HttpClientRequest;
    using Reply = HttpClientReply;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        Reply typedRun(OperationContext*) {
            const auto& cmd = request();

            auto uri = cmd.getUri();
            const bool isLocalhost = isLocalhostURI(uri);
            uassert(ErrorCodes::BadValue,
                    "URI must be either http:// or https://",
                    uri.starts_with("http://") || uri.starts_with("https://"));
            uassert(ErrorCodes::BadValue,
                    "URI must reference localhost, 127.0.0.1, or ::1",
                    isLocalhost);

            auto client = HttpClient::create();
            client->allowInsecureHTTP(isLocalhost);
            auto timeoutSecs = cmd.getTimeoutSecs();
            if (timeoutSecs) {
                client->setTimeout(Seconds(timeoutSecs.value()));
            }

            auto ret = client->request(HttpClient::HttpMethod::kGET, uri, {nullptr, 0});

            Reply reply;
            reply.setCode(ret.code);
            reply.setHeader(stringFromDataBuilder(ret.header));
            reply.setBody(stringFromDataBuilder(ret.body));
            return reply;
        }

    private:
        bool supportsWriteConcern() const final {
            return false;
        }

        void doCheckAuthorization(OperationContext*) const final {}

        NamespaceString ns() const override {
            return NamespaceString(request().getDbName());
        }
    };

    bool adminOnly() const final {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        return AllowedOnSecondary::kAlways;
    }
};

MONGO_REGISTER_COMMAND(CmdHttpClient).testOnly().forShard();

}  // namespace
}  // namespace mongo
