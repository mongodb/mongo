// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/admission/egress_response_rate_limiter.h"

#include "mongo/db/admission/egress_response_rate_limiter_gen.h"
#include "mongo/db/admission/rate_limiter_otel_metrics_recorder.h"
#include "mongo/db/client.h"
#include "mongo/db/service_context.h"
#include "mongo/otel/metrics/metric_names.h"
#include "mongo/util/decorable.h"

namespace mongo {
namespace admission {

namespace {

const auto getEgressResponseRateLimiter =
    ServiceContext::declareDecoration<boost::optional<EgressResponseRateLimiter>>();

const ConstructorActionRegistererType<ServiceContext> onServiceContextCreate{
    "InitEgressResponseRateLimiter", [](ServiceContext* ctx) {
        getEgressResponseRateLimiter(ctx).emplace();
    }};

RateLimiterOtelMetricsRecorder::MetricsSpec egressResponseRateLimiterSpec() {
    using otel::metrics::MetricNames;
    return RateLimiterOtelMetricsRecorder::MetricsSpec{
        .attemptedAdmissions = MetricNames::kEgressResponseRateLimiterAttemptedAdmissions,
        .successfulAdmissions = MetricNames::kEgressResponseRateLimiterSuccessfulAdmissions,
        .rejectedAdmissions = MetricNames::kEgressResponseRateLimiterRejectedAdmissions,
        .exemptedAdmissions = MetricNames::kEgressResponseRateLimiterExemptedAdmissions,
        .addedToQueue = MetricNames::kEgressResponseRateLimiterAddedToQueue,
        .removedFromQueue = MetricNames::kEgressResponseRateLimiterRemovedFromQueue,
        .interruptedInQueue = MetricNames::kEgressResponseRateLimiterInterruptedInQueue,
        .tokensAcquired = MetricNames::kEgressResponseRateLimiterTokensAcquired,
        .currentQueueDepth = MetricNames::kEgressResponseRateLimiterCurrentQueueDepth,
        .totalAvailableTokens = MetricNames::kEgressResponseRateLimiterTotalAvailableTokens,
        .averageTimeQueuedMicros = MetricNames::kEgressResponseRateLimiterAverageTimeQueuedMicros,
        .timeQueuedMicros = MetricNames::kEgressResponseRateLimiterTimeQueuedMicros,
    };
}

}  // namespace

EgressResponseRateLimiter::EgressResponseRateLimiter()
    : _rateLimiter{
          static_cast<double>(gEgressResponseRateLimiterRatePerSec.load()),
          gEgressResponseRateLimiterBurstCapacitySecs.load(),
          gEgressResponseRateLimiterMaxQueueDepth.load(),
          std::string(kRateLimiterName),
          RateLimiter::Options{.metricsRecorder = std::make_unique<RateLimiterOtelMetricsRecorder>(
                                   egressResponseRateLimiterSpec())}} {}

EgressResponseRateLimiter& EgressResponseRateLimiter::get(ServiceContext* svcCtx) {
    return *getEgressResponseRateLimiter(svcCtx);
}

Status EgressResponseRateLimiter::throttle(Interruptible* interruptible, ClockSource* clockSrc) {
    auto token = _rateLimiter.acquireToken();
    if (!token) {
        // Fail open: we must always send the response, even if the queue is at capacity.
        // Dropping it would corrupt the connection and hang the client.
        return Status::OK();
    }

    return std::move(*token).get(interruptible, clockSrc);
}

void EgressResponseRateLimiter::updateRateParameters(double refreshRatePerSec,
                                                     double burstCapacitySecs) {
    _rateLimiter.updateRateParameters(refreshRatePerSec, burstCapacitySecs);
}

void EgressResponseRateLimiter::updateMaxQueueDepth(std::int64_t maxQueueDepth) {
    _rateLimiter.setMaxQueueDepth(maxQueueDepth);
}

Status EgressResponseRateLimiter::onUpdateRatePerSec(std::int32_t refreshRatePerSec) {
    if (auto* client = Client::getCurrent()) {
        get(client->getServiceContext())
            .updateRateParameters(refreshRatePerSec,
                                  gEgressResponseRateLimiterBurstCapacitySecs.load());
    }
    return Status::OK();
}

Status EgressResponseRateLimiter::onUpdateBurstCapacitySecs(double burstCapacitySecs) {
    if (auto* client = Client::getCurrent()) {
        get(client->getServiceContext())
            .updateRateParameters(gEgressResponseRateLimiterRatePerSec.load(), burstCapacitySecs);
    }
    return Status::OK();
}

Status EgressResponseRateLimiter::onUpdateMaxQueueDepth(std::int64_t maxQueueDepth) {
    if (auto* client = Client::getCurrent()) {
        get(client->getServiceContext()).updateMaxQueueDepth(maxQueueDepth);
    }
    return Status::OK();
}

void EgressResponseRateLimiter::appendStats(BSONObjBuilder* bob) const {
    _rateLimiter.appendStats(bob);
}

void EgressResponseRateLimiter::installOtelMetrics(ServiceContext* svcCtx) {
    _metricsSamplingJob =
        static_cast<RateLimiterOtelMetricsRecorder&>(_rateLimiter.stats())
            .installOtelMetrics(svcCtx->getPeriodicRunner(), Seconds{1}, kRateLimiterName, [this] {
                return _rateLimiter.sampledAvailableTokens();
            });
}

const RateLimiterMetricsRecorder& EgressResponseRateLimiter::stats() const {
    return _rateLimiter.stats();
}

RateLimiterMetricsRecorder& EgressResponseRateLimiter::stats() {
    return _rateLimiter.stats();
}

double EgressResponseRateLimiter::refreshRate() const {
    return _rateLimiter.refreshRate();
}

}  // namespace admission
}  // namespace mongo
