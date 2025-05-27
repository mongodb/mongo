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

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/client/authenticate.h"
#include "mongo/client/sasl_client_session.h"
#include "mongo/db/baton.h"
#include "mongo/db/service_context.h"
#include "mongo/executor/connection_metrics.h"
#include "mongo/executor/network_connection_hook.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_severity_suppressor.h"
#include "mongo/rpc/message.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/rpc/reply_interface.h"
#include "mongo/rpc/unique_message.h"
#include "mongo/transport/baton.h"
#include "mongo/transport/message_compressor_manager.h"
#include "mongo/transport/session.h"
#include "mongo/transport/ssl_connection_context.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/duration.h"
#include "mongo/util/future.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/timer.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork

namespace mongo {

class AsyncDBClient : public std::enable_shared_from_this<AsyncDBClient> {
public:
    explicit AsyncDBClient(HostAndPort peer,
                           std::shared_ptr<transport::Session> session,
                           ServiceContext* svcCtx,
                           transport::ReactorHandle reactor)
        : _peer(std::move(peer)),
          _session(std::move(session)),
          _svcCtx(svcCtx),
          _reactor(std::move(reactor)) {}

    static Future<std::shared_ptr<AsyncDBClient>> connect(
        const HostAndPort& peer,
        transport::ConnectSSLMode sslMode,
        ServiceContext* context,
        transport::TransportLayer* tl,
        transport::ReactorHandle reactor,
        Milliseconds timeout,
        std::shared_ptr<ConnectionMetrics> connectionMetrics,
        std::shared_ptr<const transport::SSLConnectionContext> transientSSLContext = nullptr);

    Future<executor::RemoteCommandResponse> runCommandRequest(
        executor::RemoteCommandRequest request,
        const BatonHandle& baton = nullptr,
        std::shared_ptr<Timer> fromConnAcquiredTimer = nullptr,
        const CancellationToken& token = CancellationToken::uncancelable());

    Future<rpc::UniqueReply> runCommand(
        OpMsgRequest request,
        const BatonHandle& baton = nullptr,
        bool fireAndForget = false,
        std::shared_ptr<Timer> fromConnAcquiredTimer = nullptr,
        const CancellationToken& token = CancellationToken::uncancelable());

    Future<executor::RemoteCommandResponse> beginExhaustCommandRequest(
        executor::RemoteCommandRequest request,
        const BatonHandle& baton = nullptr,
        const CancellationToken& token = CancellationToken::uncancelable());

    Future<executor::RemoteCommandResponse> runExhaustCommand(
        OpMsgRequest request,
        const BatonHandle& baton = nullptr,
        const CancellationToken& token = CancellationToken::uncancelable());

    Future<executor::RemoteCommandResponse> awaitExhaustCommand(
        const BatonHandle& baton = nullptr,
        const CancellationToken& token = CancellationToken::uncancelable());

    Future<void> authenticate(const BSONObj& params);

    Future<void> authenticateInternal(
        boost::optional<std::string> mechanismHint,
        std::shared_ptr<auth::InternalAuthParametersProvider> authProvider);

    Future<bool> completeSpeculativeAuth(std::shared_ptr<SaslClientSession> session,
                                         std::string authDB,
                                         BSONObj specAuth,
                                         auth::SpeculativeAuthType speculativeAuthtype);

    Future<void> initWireVersion(const std::string& appName, executor::NetworkConnectionHook* hook);

    void cancel(const BatonHandle& baton = nullptr);

    bool isStillConnected();

    void end();

    transport::Session& getTransportSession() {
        return *_session;
    }

    const HostAndPort& remote() const;
    static constexpr Seconds kSlowConnAcquiredToWireLogSuppresionPeriod{5};

private:
    static const inline Status kCanceledStatus{ErrorCodes::CallbackCanceled,
                                               "Async network operation was canceled"};
    struct OperationState {
        OperationState(std::shared_ptr<Baton> baton, const CancellationToken& token)
            : baton(std::move(baton)), cancelSource(token) {}

        std::shared_ptr<Baton> baton;
        CancellationSource cancelSource;
        Atomic<bool> done{false};
    };

    /**
     * Helper for ensuring the future returned by the provided function can be properly cancelled
     * when the CancellationToken is cancelled. This also ensures that any cancellation callbacks
     * are fulfilled once the async work has completed.
     */
    template <typename T>
    Future<T> _withCancellation(std::shared_ptr<Baton> baton,
                                const CancellationToken& token,
                                std::function<Future<T>()> f) {
        auto opState = std::make_shared<OperationState>(baton, token);

        auto subToken = opState->cancelSource.token();
        if (subToken.isCanceled()) {
            return kCanceledStatus;
        }

        auto fut = f().onCompletion([opState](auto res) mutable {
            if (opState->done.swap(true) && res.isOK()) {
                LOGV2_DEBUG(9257004, 3, "Discarding successful result due to cancellation");
                return Future<T>::makeReady(kCanceledStatus);
            }
            return Future<T>::makeReady(std::move(res));
        });

        // Make sure to attach the cancellation callback after kicking off the async work to ensure
        // cancellation can't "miss" it.
        subToken.onCancel().unsafeToInlineFuture().getAsync(
            [this, weakSelf = weak_from_this(), weakOpState = std::weak_ptr(opState)](Status s) {
                if (!s.isOK()) {
                    return;
                }

                _reactor->schedule([this, weakSelf, weakOpState](Status s) {
                    if (!s.isOK()) {
                        return;
                    }
                    auto opState = weakOpState.lock();
                    if (!opState) {
                        return;
                    }

                    auto anchor = weakSelf.lock();
                    if (!anchor) {
                        return;
                    }

                    if (opState->done.swap(true)) {
                        return;
                    }
                    cancel(opState->baton);
                });
            });

        return fut;
    }

    Future<executor::RemoteCommandResponse> _continueReceiveExhaustResponse(
        ClockSource::StopWatch stopwatch,
        boost::optional<int32_t> msgId,
        const BatonHandle& baton = nullptr,
        const CancellationToken& token = CancellationToken::uncancelable());
    Future<Message> _waitForResponse(
        boost::optional<int32_t> msgId,
        const BatonHandle& baton = nullptr,
        const CancellationToken& token = CancellationToken::uncancelable());
    Future<void> _call(Message request,
                       int32_t msgId,
                       const BatonHandle& baton = nullptr,
                       const CancellationToken& = CancellationToken::uncancelable());
    BSONObj _buildHelloRequest(const std::string& appName, executor::NetworkConnectionHook* hook);
    void _parseHelloResponse(BSONObj request, const std::unique_ptr<rpc::ReplyInterface>& response);
    auth::RunCommandHook _makeAuthRunCommandHook();

    const HostAndPort _peer;
    std::shared_ptr<transport::Session> _session;
    ServiceContext* const _svcCtx;
    MessageCompressorManager _compressorManager;
    transport::ReactorHandle _reactor;
};

}  // namespace mongo

#undef MONGO_LOGV2_DEFAULT_COMPONENT
