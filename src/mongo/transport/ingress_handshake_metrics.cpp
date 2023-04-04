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

#include "mongo/db/commands/server_status_metric.h"
#include "mongo/db/connection_health_metrics_parameter_gen.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/logv2/log.h"
#include "mongo/util/time_support.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork

namespace mongo::transport {
namespace {
const auto getIngressHandshakeMetricsDecoration =
    Session::declareDecoration<IngressHandshakeMetrics>();

bool connHealthMetricsEnabled() {
    // (Ignore FCV check): This feature flag doesn't have any upgrade/downgrade concerns.
    return gFeatureFlagConnHealthMetrics.isEnabledAndIgnoreFCVUnsafe();
}

bool connHealthMetricsLoggingEnabled() {
    return gEnableDetailedConnectionHealthMetricLogLines;
}

CounterMetric totalTimeToFirstNonAuthCommandMillis("network.totalTimeToFirstNonAuthCommandMillis",
                                                   connHealthMetricsEnabled);
}  // namespace

IngressHandshakeMetrics& IngressHandshakeMetrics::get(Session& session) {
    return getIngressHandshakeMetricsDecoration(session);
}

void IngressHandshakeMetrics::onSessionStarted(TickSource* tickSource) {
    if (!connHealthMetricsEnabled())
        return;

    invariant(!_tickSource);
    invariant(!_sessionStartedTicks);

    _tickSource = tickSource;
    _sessionStartedTicks = _tickSource->getTicks();
}

void IngressHandshakeMetrics::onCommandReceived(const Command* command) {
    if (MONGO_likely(_firstNonAuthCommandTicks || !connHealthMetricsEnabled()))
        return;

    invariant(_sessionStartedTicks);

    auto now = _tickSource->getTicks();

    if (command->handshakeRole() != Command::HandshakeRole::kNone) {
        _lastHandshakeCommandTicks = now;

        if (command->handshakeRole() == Command::HandshakeRole::kHello) {
            _helloReceivedTime = Date_t::now();
        }

        return;
    }

    _firstNonAuthCommandTicks = now;

    auto lastAuthOrStartTicks = _lastHandshakeCommandTicks.value_or(*_sessionStartedTicks);
    auto elapsedSinceAuth = now - lastAuthOrStartTicks;

    if (connHealthMetricsLoggingEnabled()) {
        LOGV2(6788700,
              "Received first command on ingress connection since session start or auth handshake",
              "elapsed"_attr = _tickSource->ticksTo<Milliseconds>(elapsedSinceAuth));
    }

    auto elapsedSinceStart = now - *_sessionStartedTicks;
    totalTimeToFirstNonAuthCommandMillis.increment(
        _tickSource->ticksTo<Milliseconds>(elapsedSinceStart).count());
}

void IngressHandshakeMetrics::onCommandProcessed(const Command* command,
                                                 rpc::ReplyBuilderInterface* response) {
    if (MONGO_likely(_firstNonAuthCommandTicks || _helloSucceeded || !connHealthMetricsEnabled()))
        return;

    if (command->handshakeRole() != Command::HandshakeRole::kHello)
        return;

    invariant(_helloReceivedTime);

    // At this point in execution, we might not have the "ok" in the response. We could check if
    // the command succeeded by calling CommandHelpers::extractOrAppendOk(), but that would mutate
    // the response. Instead, we will simply assume that the absence of failure indicates success.
    auto body = response->getBodyBuilder();
    auto okField = body.asTempObj()["ok"];
    _helloSucceeded = (!okField) || okField.trueValue();
}

void IngressHandshakeMetrics::onResponseSent(Milliseconds processingDuration,
                                             Milliseconds sendingDuration) {
    if (MONGO_likely(_helloSucceeded || !_helloReceivedTime) || !connHealthMetricsEnabled() ||
        !connHealthMetricsLoggingEnabled())
        return;

    LOGV2(6724100,
          "Hello completed",
          "received"_attr = _helloReceivedTime,
          "processingDuration"_attr = processingDuration,
          "sendingDuration"_attr = sendingDuration,
          "okCode"_attr = _helloSucceeded ? 1.0 : 0.0);
}

void IngressHandshakeMetricsCommandHooks::onBeforeRun(OperationContext* opCtx,
                                                      const OpMsgRequest& request,
                                                      CommandInvocation* invocation) {
    if (const auto& session = opCtx->getClient()->session()) {
        IngressHandshakeMetrics::get(*session).onCommandReceived(invocation->definition());
    }
}

void IngressHandshakeMetricsCommandHooks::onAfterRun(OperationContext* opCtx,
                                                     const OpMsgRequest& request,
                                                     CommandInvocation* invocation,
                                                     rpc::ReplyBuilderInterface* response) {
    if (const auto& session = opCtx->getClient()->session()) {
        IngressHandshakeMetrics::get(*session).onCommandProcessed(invocation->definition(),
                                                                  response);
    }
}

}  // namespace mongo::transport
