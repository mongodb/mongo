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

#include "mongo/db/commands.h"
#include "mongo/db/operation_context.h"
#include "mongo/rpc/reply_builder_interface.h"
#include "mongo/transport/session.h"
#include "mongo/util/duration.h"
#include "mongo/util/tick_source.h"

#include <boost/move/utility_core.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo::transport {

/**
 * A decoration on the Session object used to capture and report the metrics around connection
 * handshake and authentication handshake for an ingress session.
 *
 * IngressHandshakeMetrics recognizes the following sequence of events:
 *
 *     session-start  auth-related-cmd* hello-cmd?  auth-related-cmd*  non-auth-related-cmd
 *
 * The state machine does not know which "auth-related-cmd" is the last until a
 * "non-auth-related-cmd" is received, and so the total duration of
 * authentication is determined retroactively when the first
 * "non-auth-related-cmd" is received.
 *
 * See the implementation for a description of the metrics published.
 *
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
     * Marks the time when the TLS (SSL) handshake with the client was completed, if the
     * connection uses TLS.
     */
    void onTLSHandshakeCompleted();

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
     * However, onCommandProcessed will never mutate the response body, and the
     * response argument is currently unused.
     */
    void onCommandProcessed(const Command* command, rpc::ReplyBuilderInterface* response);

    /**
     * This function is unused.
     */
    void onResponseSent(Microseconds processingDuration, Microseconds sendingDuration);

private:
    // The name of the state tells you which event(s) will cause a state
    // transition (except for `kDone`, which means done).
    // For example, we transition out of the `kWaitingForFirstCommand` state
    // when we receive the first command. We transition out of the
    // `kWaitingForNonAuthCommand` state when we receive a non-auth-related
    // command.
    enum class State {
        kWaitingForSessionStart,
        kWaitingForFirstCommand,
        kWaitingForHelloOrNonAuthCommand,
        kWaitingForNonAuthCommand,
        kDone
    };
    State _state = State::kWaitingForSessionStart;
    TickSource* _tickSource = nullptr;
    TickSource::Tick _sessionStartedTicks;
    TickSource::Tick _mostRecentHandshakeCommandReceivedTicks;
    TickSource::Tick _mostRecentHandshakeCommandProcessedTicks;
};

/**
 * An implementation of CommandInvocationHooks that ensures IngressHandshakeMetrics::onCommand will
 * be called for each command.
 */
class IngressHandshakeMetricsCommandHooks : public CommandInvocationHooks {
public:
    void onBeforeRun(OperationContext* opCtx, CommandInvocation* invocation) override;

    void onAfterRun(OperationContext* opCtx,
                    CommandInvocation* invocation,
                    rpc::ReplyBuilderInterface* response) override;
};

}  // namespace mongo::transport
