/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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
    if (_rateLimiter->tryAcquireToken().isOK()) {
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

const admission::RateLimiter::Stats& FlowControlRateLimiter::stats() const {
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
