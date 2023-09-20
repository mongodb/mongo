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

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "mongo/bson/bsonobj.h"
#include "mongo/client/authenticate.h"
#include "mongo/client/sasl_client_session.h"
#include "mongo/db/baton.h"
#include "mongo/db/service_context.h"
#include "mongo/executor/connection_metrics.h"
#include "mongo/executor/network_connection_hook.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/remote_command_response.h"
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
#include "mongo/util/clock_source.h"
#include "mongo/util/duration.h"
#include "mongo/util/future.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/timer.h"

namespace mongo {

class AsyncDBClient : public std::enable_shared_from_this<AsyncDBClient> {
public:
    explicit AsyncDBClient(const HostAndPort& peer,
                           std::shared_ptr<transport::Session> session,
                           ServiceContext* svcCtx)
        : _peer(std::move(peer)), _session(std::move(session)), _svcCtx(svcCtx) {}

    using Handle = std::shared_ptr<AsyncDBClient>;

    static Future<Handle> connect(
        const HostAndPort& peer,
        transport::ConnectSSLMode sslMode,
        ServiceContext* context,
        transport::ReactorHandle reactor,
        Milliseconds timeout,
        std::shared_ptr<ConnectionMetrics> connectionMetrics,
        std::shared_ptr<const transport::SSLConnectionContext> transientSSLContext = nullptr);

    Future<executor::RemoteCommandResponse> runCommandRequest(
        executor::RemoteCommandRequest request,
        const BatonHandle& baton = nullptr,
        boost::optional<std::shared_ptr<Timer>> fromConnAcquiredTimer = boost::none);
    Future<rpc::UniqueReply> runCommand(
        OpMsgRequest request,
        const BatonHandle& baton = nullptr,
        bool fireAndForget = false,
        boost::optional<std::shared_ptr<Timer>> fromConnAcquiredTimer = boost::none);

    Future<executor::RemoteCommandResponse> beginExhaustCommandRequest(
        executor::RemoteCommandRequest request, const BatonHandle& baton = nullptr);
    Future<executor::RemoteCommandResponse> runExhaustCommand(OpMsgRequest request,
                                                              const BatonHandle& baton = nullptr);
    Future<executor::RemoteCommandResponse> awaitExhaustCommand(const BatonHandle& baton = nullptr);

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

    const HostAndPort& remote() const;
    static constexpr Seconds kSlowConnAcquiredToWireLogSuppresionPeriod{5};

private:
    Future<executor::RemoteCommandResponse> _continueReceiveExhaustResponse(
        ClockSource::StopWatch stopwatch,
        boost::optional<int32_t> msgId,
        const BatonHandle& baton = nullptr);
    Future<Message> _waitForResponse(boost::optional<int32_t> msgId,
                                     const BatonHandle& baton = nullptr);
    Future<void> _call(Message request, int32_t msgId, const BatonHandle& baton = nullptr);
    BSONObj _buildHelloRequest(const std::string& appName, executor::NetworkConnectionHook* hook);
    void _parseHelloResponse(BSONObj request, const std::unique_ptr<rpc::ReplyInterface>& response);
    auth::RunCommandHook _makeAuthRunCommandHook();

    const HostAndPort _peer;
    std::shared_ptr<transport::Session> _session;
    ServiceContext* const _svcCtx;
    MessageCompressorManager _compressorManager;
};

}  // namespace mongo
