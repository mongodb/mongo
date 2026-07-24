// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
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
#include "mongo/otel/telemetry_context.h"
#include "mongo/otel/traces/span/span.h"
#include "mongo/otel/traces/span/span_names.h"
#include "mongo/rpc/message.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/rpc/reply_interface.h"
#include "mongo/rpc/telemetry_context_section_gen.h"
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
#include <string_view>
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

    Future<void> authenticate(const auth::Credential& credential);

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

    /**
     * Returns the TelemetryContextSection to attach to the egress OpMsg for `request`. Returns
     * boost::none if the target does not support it (wire version < WIRE_VERSION_90 or INT_MAX
     * sentinel) or if no active span is present in the request's telemetry context.
     *
     * This is a static helper to allow unit testing without a live connection.
     */
    [[nodiscard]] static boost::optional<TelemetryContextSection> makeEgressTelemetrySection(
        const executor::RemoteCommandRequest& request, int maxWireVersion);

    /**
     * Starts a Span for an outgoing `commandName`, registering the command name if needed and
     * falling back to the generic `span_names::kMongoRPC` only when no command name is available.
     * The span is started on `telemetryContext`, which is mutated in place: if it was null and a
     * new trace should be started, it will be populated with a newly created TelemetryContext so
     * that the caller can propagate it (e.g. via `makeEgressTelemetrySection`). Uses CLIENT span
     * kind by default, or PRODUCER when `fireAndForget` is true (moreToCome on the wire).
     *
     * This is a static helper to allow unit testing without a live connection.
     */
    [[nodiscard]] static otel::traces::Span startEgressSpan(
        std::shared_ptr<otel::TelemetryContext>& telemetryContext,
        std::string_view commandName,
        bool fireAndForget);

    /**
     * If `isMoreToComeSet` is false and `span` holds a value, sets `status` on the span and then
     * ends it (by resetting `span`). Otherwise, leaves `span` untouched. Returns whether the span
     * was ended.
     *
     * This is a static helper to allow unit testing without a live connection.
     */
    static bool maybeEndExhaustSpan(boost::optional<otel::traces::Span>& span,
                                    bool isMoreToComeSet,
                                    const Status& status);

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
    int _negotiatedMaxWireVersion = 0;

    // The Span covering the full exhaust stream for the command currently in flight on this
    // connection, if any. Started in `beginExhaustCommandRequest` and ended (via
    // `maybeEndExhaustSpan`) in `_continueReceiveExhaustResponse` once a response with
    // `isMoreToComeSet == false` is received, or once the exhaust stream fails.
    boost::optional<otel::traces::Span> _exhaustSpan;
};

}  // namespace mongo

#undef MONGO_LOGV2_DEFAULT_COMPONENT
