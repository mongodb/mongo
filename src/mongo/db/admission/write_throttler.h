/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/admission/rate_limiter.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/util/modules.h"

#include <cstdint>
#include <memory>

namespace mongo {

class TickSource;

/**
 * A generic write-admission gate that slows writes to a configurable target rate. It wraps an
 * admission::RateLimiter (the same token-bucket mechanism Flow Control uses when
 * flowControlUseRateLimiter is enabled) and sits at service entry on the write path: each write
 * command must clear this gate before proceeding.
 *
 * The target rate is configured through the writeThrottlerTargetRatePerSec server parameter (set
 * via setParameter); the on_update handler pushes the new rate into the token bucket. Until the
 * rate is lowered below kMaxRate, the throttler stays idle (no throttling). The throttler is always
 * batch-aware: it charges one token per admission and reconciles the remaining accumulated
 * document cost after the command completes, so it meters documents/sec rather than operations/sec.
 * This object owns no threads; it is a thin mechanism plus the observability state surfaced via
 * serverStatus/FTDC.
 *
 * See src/mongo/db/admission/README.md for the full design.
 */
class [[MONGO_MOD_PUBLIC]] WriteThrottler {
public:
    static constexpr int kMaxRate = 1000 * 1000 * 1000;

    WriteThrottler();
    explicit WriteThrottler(TickSource* tickSource);

    /**
     * Admission gate on the write hot path. Always forwards to the rate limiter (matching the
     * always-on ingress rate limiter): it blocks interruptibly via opCtx until a token is
     * available, recording the wait on the WriteThrottlerAdmissionContext so curOp/serverStatus
     * report the write-throttle queue. When the throttler is idle (target rate == kMaxRate) the
     * token bucket admits immediately. Rejects with RateLimitExceeded if the queue depth is
     * exceeded.
     */
    void admitOperation(OperationContext* opCtx);

    double tokenBalance_forTest() const {
        return _rateLimiter->tokenBalance();
    }

    /**
     * Finalizes write-throttle admission for a completed command. This reconciles the
     * one-token-per-admission upfront charge with the accumulated successful document count from
     * the WriteThrottlerAdmissionContext, debiting the remaining cost (capped by
     * writeThrottlerMaxCostPerOp) from the bucket so the throttler meters documents/sec rather than
     * operations/sec.
     */
    void finalizeAdmission(OperationContext* opCtx);

    void appendStats(BSONObjBuilder* bob) const;

    /** Builds the "writeThrottler" sub-document under serverStatus.queues. */
    BSONObj generateSection() const;

    static WriteThrottler* get(ServiceContext* service);
    static WriteThrottler* get(OperationContext* opCtx);
    static void set(ServiceContext* service, std::unique_ptr<WriteThrottler> throttler);

    /**
     * Called automatically when the writeThrottlerTargetRatePerSec server parameter changes.
     * Mirrors IngressRequestRateLimiter::onUpdateAdmissionRatePerSec. When the throttler is
     * enabled, pushes the new target rate into the token bucket; otherwise it is a no-op (inert
     * when off).
     */
    [[MONGO_MOD_PRIVATE]] static Status onUpdateTargetRatePerSec(int32_t targetRatePerSec);

    /**
     * Called automatically when the writeThrottlerEnabled server parameter changes. Belt-and-
     * suspenders sync of the rate with the enabled state: toggling false disarms the bucket
     * (kMaxRate); toggling true pushes the current target rate (gWriteThrottlerTargetRatePerSec).
     */
    [[MONGO_MOD_PRIVATE]] static Status onUpdateEnabled(bool enabled);

    /**
     * Called automatically when the writeThrottlerBurstCapacitySecs server parameter changes.
     * Mirrors IngressRequestRateLimiter::onUpdateAdmissionBurstCapacitySecs. Re-applies only the
     * burst capacity to the token bucket, preserving the current effective rate.
     */
    [[MONGO_MOD_PRIVATE]] static Status onUpdateBurstCapacitySecs(double burstCapacitySecs);

    /**
     * Called automatically when the writeThrottlerMaxQueueDepth server parameter changes.
     * Mirrors IngressRequestRateLimiter::onUpdateAdmissionMaxQueueDepth. Re-applies only the queue
     * depth, preserving the current rate.
     */
    [[MONGO_MOD_PRIVATE]] static Status onUpdateMaxQueueDepth(long long maxQueueDepth);

private:
    /**
     * Updates the token rate. Reads the burst-capacity and max-queue-depth server parameters.
     * Called by the on_update handlers below when the target-rate/enabled server parameters change.
     */
    void updateRate(int targetRatePerSec);

    std::unique_ptr<admission::RateLimiter> _rateLimiter;
};

}  // namespace mongo
