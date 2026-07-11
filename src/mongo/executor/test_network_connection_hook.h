// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status_with.h"
#include "mongo/executor/network_connection_hook.h"

#include <memory>

#include <boost/optional.hpp>

namespace mongo {
namespace executor {


/**
 * A utility for creating one-off NetworkConnectionHook instances from inline lambdas. This is
 * only to be used in testing code, not in production.
 */
template <typename ValidateFunc, typename RequestFunc, typename ReplyFunc>
class TestConnectionHook final : public NetworkConnectionHook {
public:
    TestConnectionHook(ValidateFunc&& validateFunc,
                       RequestFunc&& requestFunc,
                       ReplyFunc&& replyFunc)
        : _validateFunc(std::forward<ValidateFunc>(validateFunc)),
          _requestFunc(std::forward<RequestFunc>(requestFunc)),
          _replyFunc(std::forward<ReplyFunc>(replyFunc)) {}

    Status validateHost(const HostAndPort& remoteHost,
                        const BSONObj& request,
                        const RemoteCommandResponse& helloReply) override {
        return _validateFunc(remoteHost, request, helloReply);
    }

    StatusWith<boost::optional<RemoteCommandRequest>> makeRequest(
        const HostAndPort& remoteHost) override {
        return _requestFunc(remoteHost);
    }

    Status handleReply(const HostAndPort& remoteHost, RemoteCommandResponse&& response) override {
        return _replyFunc(remoteHost, std::move(response));
    }

private:
    ValidateFunc _validateFunc;
    RequestFunc _requestFunc;
    ReplyFunc _replyFunc;
};

/**
 * Factory function for TestConnectionHook instances. Needed for template type deduction, so that
 * one can instantiate a TestConnectionHook instance without uttering the unutterable (types).
 */
template <typename Val, typename Req, typename Rep>
std::unique_ptr<TestConnectionHook<Val, Req, Rep>> makeTestHook(Val&& validateFunc,
                                                                Req&& requestFunc,
                                                                Rep&& replyFunc) {
    return std::make_unique<TestConnectionHook<Val, Req, Rep>>(std::forward<Val>(validateFunc),
                                                               std::forward<Req>(requestFunc),
                                                               std::forward<Rep>(replyFunc));
}

}  // namespace executor
}  // namespace mongo
