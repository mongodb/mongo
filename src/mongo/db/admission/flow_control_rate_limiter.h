// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/admission/rate_limiter.h"
#include "mongo/db/flow_control_ticketholder.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/util/modules.h"

#include <cstdint>
#include <memory>

namespace mongo {

/**
 * Wraps admission::RateLimiter with a flow-control-specific interface, providing smooth
 * token-bucket-based throttling as an alternative to FlowControlTicketholder's batch-refill
 * semaphore approach. Unconditionally registered as a ServiceContext decoration by the
 * FlowControl constructor; gated at the call site by flowControlUseRateLimiter.
 */
class [[MONGO_MOD_PUBLIC]] FlowControlRateLimiter {
public:
    static constexpr int64_t kDefaultMaxQueueDepth = 1000000;
    static constexpr int kMaxRate = 1000 * 1000 * 1000;

    FlowControlRateLimiter();

    /**
     * Updates the token rate. Called every 1s by the FlowControl periodic job.
     * Reads flowControlRateLimiterBurstCapacitySecs server parameter for burst capacity.
     */
    void updateRate(int targetRateLimit);

    /**
     * Blocks until a token is available, mapping timing stats to CurOp fields for backward
     * compatibility with existing FTDC metrics. Interruptible via opCtx.
     */
    void acquireTicket(OperationContext* opCtx, FlowControlTicketholder::CurOp* curOp);

    const admission::RateLimiterMetricsRecorder& stats() const;
    int64_t queued() const;
    void appendStats(BSONObjBuilder* bob) const;

    static FlowControlRateLimiter* get(ServiceContext* service);
    static FlowControlRateLimiter* get(OperationContext* opCtx);
    static void set(ServiceContext* service, std::unique_ptr<FlowControlRateLimiter> rl);

private:
    std::unique_ptr<admission::RateLimiter> _rateLimiter;
};

}  // namespace mongo
