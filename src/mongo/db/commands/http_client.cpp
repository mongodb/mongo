// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/net/http_client.h"

#include "mongo/base/data_builder.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/init.h"  // IWYU pragma: keep
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
#include <string_view>

#include <boost/optional/optional.hpp>

namespace mongo {
namespace {

bool isLocalhostURI(std::string_view uri) {
    std::string_view host;

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
