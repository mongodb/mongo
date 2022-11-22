/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include <boost/optional.hpp>

#include "mongo/db/commands.h"
#include "mongo/transport/session.h"
#include "mongo/util/duration.h"
#include "mongo/util/tick_source.h"

namespace mongo::transport {

/**
 * A decoration on the Session object used to capture and report the metrics around connection
 * handshake and authentication handshake for an ingress session.
 */
class IngressHandshakeMetrics {
public:
    static IngressHandshakeMetrics& get(Session& session);

    /**
     * Marks the time when the session was started. The clock source and tick source must remain
     * valid for at least as long as the last method call on this instance.
     */
    void onSessionStarted(TickSource* tickSource);

    /**
     * Checks if the command is part of the handshake conversation and, if so, records the
     * observations necessary to generate the associated metrics. If this is the first non-auth
     * command received on this session, it reports the session auth handshake metrics.
     * Note: for the purposes of these metrics, hello commands are also considered to be part of the
     * initial authentication conversation.
     */
    void onCommandReceived(const Command* command);

    /**
     * Checks if the command is a hello command and, if so, records the observations necessary to
     * generate the associated metrics. Note that the response argument is not const, because the
     * ReplyBuilderInterface does not expose any const methods to inspect the response body.
     * However, onCommandProcessed does not mutate the response body.
     */
    void onCommandProcessed(const Command* command, rpc::ReplyBuilderInterface* response);

    /**
     * If the response was just sent for the first hello command for this session, reports the
     * associated metrics. The processing duration and sending duration are already being measured
     * in SessionWorkflowMetrics, so they are expected as arguments, rather than having this class
     * record them separately.
     */
    void onResponseSent(Milliseconds processingDuration, Milliseconds sendingDuration);

private:
    TickSource* _tickSource{nullptr};
    boost::optional<TickSource::Tick> _sessionStartedTicks;
    boost::optional<Date_t> _helloReceivedTime;
    boost::optional<bool> _helloSucceeded;
    boost::optional<TickSource::Tick> _lastHandshakeCommandTicks;
    boost::optional<TickSource::Tick> _firstNonAuthCommandTicks;
};

/**
 * An implementation of CommandInvocationHooks that ensures IngressHandshakeMetrics::onCommand will
 * be called for each command.
 */
class IngressHandshakeMetricsCommandHooks : public CommandInvocationHooks {
public:
    void onBeforeRun(OperationContext* opCtx,
                     const OpMsgRequest& request,
                     CommandInvocation* invocation) override;

    void onAfterRun(OperationContext* opCtx,
                    const OpMsgRequest& request,
                    CommandInvocation* invocation,
                    rpc::ReplyBuilderInterface* response) override;
};

}  // namespace mongo::transport
