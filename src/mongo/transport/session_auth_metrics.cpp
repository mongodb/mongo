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
#include "mongo/transport/session_auth_metrics.h"

#include "mongo/db/commands/server_status_metric.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/logv2/log.h"
#include "mongo/util/duration.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork

namespace mongo::transport {
namespace {
const auto getSessionAuthMetricsDecoration = Session::declareDecoration<SessionAuthMetrics>();

bool connHealthMetricsEnabled() {
    return gFeatureFlagConnHealthMetrics.isEnabledAndIgnoreFCV();
}

CounterMetric totalTimeToFirstNonAuthCommandMillis("network.totalTimeToFirstNonAuthCommandMillis",
                                                   connHealthMetricsEnabled);
}  // namespace

SessionAuthMetrics& SessionAuthMetrics::get(Session& session) {
    return getSessionAuthMetricsDecoration(session);
}

void SessionAuthMetrics::onSessionStarted(ClockSource* clockSource) {
    if (!connHealthMetricsEnabled())
        return;

    invariant(!_clockSource);
    invariant(!_sessionStartedTime);
    _clockSource = clockSource;
    _sessionStartedTime = _clockSource->now();
}

void SessionAuthMetrics::onCommand(const Command* command) {
    if (MONGO_likely(_firstNonAuthCommandTime || !connHealthMetricsEnabled()))
        return;

    invariant(_clockSource);
    invariant(_sessionStartedTime);

    auto now = _clockSource->now();

    if (command->isPartOfAuthHandshake()) {
        _authHandshakeFinishedTime = now;
        return;
    }

    _firstNonAuthCommandTime = now;

    auto lastAuthOrStartTime = _authHandshakeFinishedTime.value_or(_sessionStartedTime.value());
    Milliseconds elapsedSinceAuth = now - lastAuthOrStartTime;
    LOGV2(6788700,
          "Received first command on ingress connection since session start or auth handshake",
          "elapsed"_attr = elapsedSinceAuth);

    Milliseconds elapsedSinceStart = now - _sessionStartedTime.value();
    totalTimeToFirstNonAuthCommandMillis.increment(elapsedSinceStart.count());
}

void SessionAuthMetricsCommandHooks::onBeforeRun(OperationContext* opCtx,
                                                 const OpMsgRequest& request,
                                                 CommandInvocation* invocation) {
    if (const auto& session = opCtx->getClient()->session()) {
        transport::SessionAuthMetrics::get(*session).onCommand(invocation->definition());
    }
}

}  // namespace mongo::transport
