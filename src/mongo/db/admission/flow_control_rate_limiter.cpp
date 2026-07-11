// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/admission/flow_control_rate_limiter.h"

#include "mongo/db/admission/flow_control_parameters_gen.h"
#include "mongo/util/decorable.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/timer.h"

#include <algorithm>

namespace mongo {

namespace {
const auto getFlowControlRateLimiterDecoration =
    ServiceContext::declareDecoration<std::unique_ptr<FlowControlRateLimiter>>();
}  // namespace

FlowControlRateLimiter::FlowControlRateLimiter()
    : _rateLimiter(
          std::make_unique<admission::RateLimiter>(static_cast<double>(kMaxRate),
                                                   gFlowControlRateLimiterBurstCapacitySecs.load(),
                                                   gFlowControlRateLimiterMaxQueueDepth.load(),
                                                   "flowControl")) {}

void FlowControlRateLimiter::updateRate(int targetRateLimit) {
    auto rate = std::max(1, targetRateLimit);
    _rateLimiter->updateRateParameters(static_cast<double>(rate),
                                       gFlowControlRateLimiterBurstCapacitySecs.load());
    _rateLimiter->setMaxQueueDepth(gFlowControlRateLimiterMaxQueueDepth.load());
}

void FlowControlRateLimiter::acquireTicket(OperationContext* opCtx,
                                           FlowControlTicketholder::CurOp* curOp) {
    // TODO SERVER-126182: Use DeferredToken to avoid the tryAcquireToken/acquireToken
    // double-call and the resulting internal stats double-count.
    if (_rateLimiter->tryAcquireToken()) {
        curOp->ticketsAcquired++;
        return;
    }

    curOp->acquireWaitCount++;
    curOp->waiting = true;
    Timer timer;
    ON_BLOCK_EXIT([&] {
        curOp->timeAcquiringMicros += timer.micros();
        curOp->waiting = false;
    });

    uassertStatusOK(_rateLimiter->acquireToken(opCtx));
    curOp->ticketsAcquired++;
}

const admission::RateLimiterMetricsRecorder& FlowControlRateLimiter::stats() const {
    return _rateLimiter->stats();
}

int64_t FlowControlRateLimiter::queued() const {
    return _rateLimiter->queued();
}

void FlowControlRateLimiter::appendStats(BSONObjBuilder* bob) const {
    _rateLimiter->appendStats(bob);
}

FlowControlRateLimiter* FlowControlRateLimiter::get(ServiceContext* service) {
    return getFlowControlRateLimiterDecoration(service).get();
}

FlowControlRateLimiter* FlowControlRateLimiter::get(OperationContext* opCtx) {
    return get(opCtx->getServiceContext());
}

void FlowControlRateLimiter::set(ServiceContext* service,
                                 std::unique_ptr<FlowControlRateLimiter> rl) {
    getFlowControlRateLimiterDecoration(service) = std::move(rl);
}

}  // namespace mongo
