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
class MONGO_MOD_PUBLIC FlowControlRateLimiter {
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

    const admission::RateLimiter::Stats& stats() const;
    int64_t queued() const;
    void appendStats(BSONObjBuilder* bob) const;

    static FlowControlRateLimiter* get(ServiceContext* service);
    static FlowControlRateLimiter* get(OperationContext* opCtx);
    static void set(ServiceContext* service, std::unique_ptr<FlowControlRateLimiter> rl);

private:
    std::unique_ptr<admission::RateLimiter> _rateLimiter;
};

}  // namespace mongo
