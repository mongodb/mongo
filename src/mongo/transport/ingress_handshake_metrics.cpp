// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#include "mongo/transport/ingress_handshake_metrics.h"

#include "mongo/base/counter.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/commands/server_status/histogram_server_status_metric.h"
#include "mongo/db/commands/server_status/server_status_metric.h"
#include "mongo/db/connection_health_metrics_parameter_gen.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/logv2/log.h"
#include "mongo/otel/metrics/metrics_histogram.h"
#include "mongo/otel/metrics/metrics_service.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/moving_average_metric.h"
#include "mongo/util/tick_source.h"

#include <memory>
#include <utility>
#include <vector>

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

auto& totalIngressTLSConnections =  //
    *MetricBuilder<Counter64>("network.totalIngressTLSConnections");
auto& totalIngressTLSHandshakeTimeMillis =  //
    *MetricBuilder<Counter64>("network.totalIngressTLSHandshakeTimeMillis");

otel::metrics::Histogram<int64_t>& ingressTLSHandshakeTimesMicros =
    otel::metrics::MetricsService::instance().createInt64Histogram(
        otel::metrics::MetricNames::kIngressTLSHandshakeLatency,
        "The latency of the TLS handshake when establishing a new ingress connection.",
        otel::metrics::MetricUnit::kMicroseconds,
        {.serverStatusOptions =
             otel::metrics::ServerStatusOptions{.dottedPath = "network.ingressTLSHandshakeMicros",
                                                .role = ClusterRole::None},
         .explicitBucketBoundaries = HistogramServerStatusMetric::computeBounds<double>(10, 1, 5),
         .serializationFormat = otel::metrics::HistogramSerializationFormat::kBucketCounts});

otel::metrics::Histogram<int64_t>& ingressHelloTimesMicros =
    otel::metrics::MetricsService::instance().createInt64Histogram(
        otel::metrics::MetricNames::kIngressHelloLatency,
        "The latency of the hello command when establishing a new ingress connection.",
        otel::metrics::MetricUnit::kMicroseconds,
        {.serverStatusOptions =
             otel::metrics::ServerStatusOptions{.dottedPath = "network.ingressHelloLatency",
                                                .role = ClusterRole::None},
         .explicitBucketBoundaries = HistogramServerStatusMetric::computeBounds<double>(10, 1, 5),
         .serializationFormat = otel::metrics::HistogramSerializationFormat::kBucketCounts});

otel::metrics::Histogram<int64_t>& ingressAuthTimesMicros =
    otel::metrics::MetricsService::instance().createInt64Histogram(
        otel::metrics::MetricNames::kIngressAuthLatency,
        "The latency of auth when establishing a new ingress connection.",
        otel::metrics::MetricUnit::kMicroseconds,
        {.serverStatusOptions =
             otel::metrics::ServerStatusOptions{.dottedPath = "network.ingressAuthLatency",
                                                .role = ClusterRole::None},
         .explicitBucketBoundaries = HistogramServerStatusMetric::computeBounds<double>(10, 1, 5),
         .serializationFormat = otel::metrics::HistogramSerializationFormat::kBucketCounts});
}  // namespace

IngressHandshakeMetrics& IngressHandshakeMetrics::get(Session& session) {
    return getIngressHandshakeMetricsDecoration(session);
}

void IngressHandshakeMetrics::onSessionStarted(TickSource* tickSource) {
    invariant(_state == State::kWaitingForSessionStart);
    _tickSource = tickSource;
    // Ensure that we understand the session startpoint as the earliest known handshake.
    _sessionStartedTicks = _mostRecentHandshakeCommandReceivedTicks =
        _mostRecentHandshakeCommandProcessedTicks = _tickSource->getTicks();
    _state = State::kWaitingForFirstCommand;
}

void IngressHandshakeMetrics::onTLSHandshakeStarted(TickSource* tickSource) {
    _tlsHandshakeTickSource = tickSource;
    _tlsHandshakeStartedTicks = tickSource->getTicks();
}

void IngressHandshakeMetrics::onTLSHandshakeCompleted() {
    if (_tlsHandshakeTickSource) {
        const auto duration = _tlsHandshakeTickSource->ticksTo<Microseconds>(
            _tlsHandshakeTickSource->getTicks() - _tlsHandshakeStartedTicks);
        const auto durationMicros = duration.count();
        const auto durationMillis = duration_cast<Milliseconds>(duration).count();
        if (connHealthMetricsLoggingEnabled()) {
            LOGV2(
                6723804, "Ingress TLS handshake complete", "durationMillis"_attr = durationMillis);
        }
        totalIngressTLSConnections.increment(1);
        totalIngressTLSHandshakeTimeMillis.increment(durationMillis);
        ingressTLSHandshakeTimesMicros.record(durationMicros);
    }

    if (_state == State::kWaitingForSessionStart) {
        // We're being called from inside of a unit test that isn't using
        // `SessionManagerCommon::startSession`. The session-relative metrics don't apply.
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
        // A non-auth command has already ended the handshake, but the session may still
        // authenticate afterwards.
        if (_tickSource && !_authCommandReceivedTicks &&
            command->handshakeRole() == Command::HandshakeRole::kAuth) {
            _authCommandReceivedTicks = _tickSource->getTicks();
        }
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

            if (_mostRecentHandshakeCommandProcessedTicks > _sessionStartedTicks) {
                const auto micros = _tickSource->ticksTo<Microseconds>(
                    _mostRecentHandshakeCommandProcessedTicks - _sessionStartedTicks);
                averageTimeToCompletedAuthMicros.addSample(micros.count());
            }
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
        case State::kWaitingForNonAuthCommand: {
            const auto role = command->handshakeRole();
            _mostRecentHandshakeCommandReceivedTicks = now;
            if (role == Command::HandshakeRole::kHello) {
                _helloCommandReceivedTicks = now;
            }
            if (!_authCommandReceivedTicks && role == Command::HandshakeRole::kAuth) {
                _authCommandReceivedTicks = now;
            }
            return;
        }
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
                ingressHelloTimesMicros.record(
                    _tickSource->ticksTo<Microseconds>(now - _helloCommandReceivedTicks).count());
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

void IngressHandshakeMetrics::onAuthCompleted(const Command* command) {
    if (_authCompletionRecorded) {
        // Only record one sample per session.
        return;
    }
    _authCompletionRecorded = true;
    if (command->handshakeRole() != Command::HandshakeRole::kAuth) {
        // Authentication completed via a hello carrying speculative authentication; that latency
        // is already captured by the hello histogram.
        return;
    }

    invariant(_tickSource);
    if (!_authCommandReceivedTicks) {
        return;
    }
    ingressAuthTimesMicros.record(
        _tickSource->ticksTo<Microseconds>(_tickSource->getTicks() - _authCommandReceivedTicks)
            .count());
}

void IngressHandshakeMetrics::onResponseSent(/*processingDuration=*/Microseconds,
                                             /*sendingDuration=*/Microseconds) {
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
        auto& metrics = IngressHandshakeMetrics::get(*session);
        metrics.onCommandProcessed(invocation->definition(), response);

        // A command finishing while the session is authenticated marks the completion of
        // authentication on this connection.
        if (AuthorizationSession::get(opCtx->getClient())->isAuthenticated()) {
            metrics.onAuthCompleted(invocation->definition());
        }
    }
}

}  // namespace mongo::transport
