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
#include "mongo/transport/ingress_handshake_metrics.h"

#include "mongo/base/counter.h"
#include "mongo/db/client.h"
#include "mongo/db/commands/server_status_metric.h"
#include "mongo/db/connection_health_metrics_parameter_gen.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/moving_average_metric.h"
#include "mongo/util/tick_source.h"

#include <memory>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork

namespace mongo::transport {
namespace {
const auto getIngressHandshakeMetricsDecoration =
    Session::declareDecoration<IngressHandshakeMetrics>();

bool connHealthMetricsLoggingEnabled() {
    return gEnableDetailedConnectionHealthMetricLogLines.load();
}

auto& totalTimeToFirstNonAuthCommandMillis =
    *MetricBuilder<Counter64>("network.totalTimeToFirstNonAuthCommandMillis");
// `averageTimeToCompletedAuthMicros` tracks the time from when the session is
// started until the last handshake-related command is finished running.
auto& averageTimeToCompletedAuthMicros =
    *MetricBuilder<MovingAverageMetric>("network.averageTimeToCompletedAuthMicros").bind(0.2);
// `averageTimeToCompletedHelloMicros` tracks the time from when the session is
// started until when the hello command is finished running.
auto& averageTimeToCompletedHelloMicros =
    *MetricBuilder<MovingAverageMetric>("network.averageTimeToCompletedHelloMicros").bind(0.2);
// `averageTimeToCompletedTLSHandshakeMicros` tracks the time from when the
// session is started until when the TLS (SSL) handshake with the client has
// completed. It's only relevant for secure connections.
auto& averageTimeToCompletedTLSHandshakeMicros =
    *MetricBuilder<MovingAverageMetric>("network.averageTimeToCompletedTLSHandshakeMicros")
         .bind(0.2);
}  // namespace

IngressHandshakeMetrics& IngressHandshakeMetrics::get(Session& session) {
    return getIngressHandshakeMetricsDecoration(session);
}

void IngressHandshakeMetrics::onSessionStarted(TickSource* tickSource) {
    invariant(_state == State::kWaitingForSessionStart);
    _tickSource = tickSource;
    _sessionStartedTicks = _tickSource->getTicks();
    _state = State::kWaitingForFirstCommand;
}

void IngressHandshakeMetrics::onTLSHandshakeCompleted() {
    if (_state == State::kWaitingForSessionStart) {
        // We're being called from inside of a unit test that isn't using
        // `SessionManagerCommon::startSession`. Pretend that we don't exist.
        return;
    }
    invariant(_state == State::kWaitingForFirstCommand);
    invariant(_tickSource);
    const auto micros =
        _tickSource->ticksTo<Microseconds>(_tickSource->getTicks() - _sessionStartedTicks);
    averageTimeToCompletedTLSHandshakeMicros.addSample(micros.count());
}

void IngressHandshakeMetrics::onCommandReceived(const Command* command) {
    if (_state == State::kDone) {
        return;
    }

    invariant(_tickSource);
    const TickSource::Tick now = _tickSource->getTicks();

    if (command->handshakeRole() == Command::HandshakeRole::kNone) {
        TickSource::Tick relativeStart;
        if (_state == State::kWaitingForFirstCommand) {
            relativeStart = _sessionStartedTicks;
        } else {
            relativeStart = _mostRecentHandshakeCommandReceivedTicks;
            const auto micros = _tickSource->ticksTo<Microseconds>(
                _mostRecentHandshakeCommandProcessedTicks - _sessionStartedTicks);
            averageTimeToCompletedAuthMicros.addSample(micros.count());
        }

        if (connHealthMetricsLoggingEnabled()) {
            LOGV2(6788700,
                  "Received first command on ingress connection since session start or auth "
                  "handshake",
                  "elapsed"_attr = _tickSource->ticksTo<Milliseconds>(now - relativeStart));
        }
        totalTimeToFirstNonAuthCommandMillis.increment(
            _tickSource->ticksTo<Milliseconds>(now - _sessionStartedTicks).count());

        _state = State::kDone;
        return;
    }

    switch (_state) {
        case State::kWaitingForFirstCommand:
            _state = State::kWaitingForHelloOrNonAuthCommand;
            [[fallthrough]];
        case State::kWaitingForHelloOrNonAuthCommand:
        case State::kWaitingForNonAuthCommand:
            _mostRecentHandshakeCommandReceivedTicks = now;
            return;
        default:
            MONGO_UNREACHABLE;
    }
}

void IngressHandshakeMetrics::onCommandProcessed(const Command* command,
                                                 rpc::ReplyBuilderInterface* /*response*/) {
    if (_state == State::kDone) {
        return;
    }

    invariant(_tickSource);
    const auto now = _tickSource->getTicks();
    switch (_state) {
        case State::kWaitingForHelloOrNonAuthCommand:
            if (command->handshakeRole() == Command::HandshakeRole::kHello) {
                const auto micros = _tickSource->ticksTo<Microseconds>(now - _sessionStartedTicks);
                averageTimeToCompletedHelloMicros.addSample(micros.count());
                _state = State::kWaitingForNonAuthCommand;
            }
            [[fallthrough]];
        case State::kWaitingForNonAuthCommand:
            _mostRecentHandshakeCommandProcessedTicks = now;
            return;
        default:
            MONGO_UNREACHABLE;
    }
}

void IngressHandshakeMetrics::onResponseSent(Microseconds /*processingDuration*/,
                                             Microseconds /*sendingDuration*/) {
    // TODO(SERVER-63883): This function is never invoked.
}

void IngressHandshakeMetricsCommandHooks::onBeforeRun(OperationContext* opCtx,
                                                      CommandInvocation* invocation) {
    if (const auto& session = opCtx->getClient()->session()) {
        IngressHandshakeMetrics::get(*session).onCommandReceived(invocation->definition());
    }
}

void IngressHandshakeMetricsCommandHooks::onAfterRun(OperationContext* opCtx,
                                                     CommandInvocation* invocation,
                                                     rpc::ReplyBuilderInterface* response) {
    if (const auto& session = opCtx->getClient()->session()) {
        IngressHandshakeMetrics::get(*session).onCommandProcessed(invocation->definition(),
                                                                  response);
    }
}

}  // namespace mongo::transport
