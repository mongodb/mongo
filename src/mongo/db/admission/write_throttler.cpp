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

#include "mongo/db/admission/write_throttler.h"

#include "mongo/db/admission/ticketing/admission_context.h"
#include "mongo/db/admission/write_throttler_admission_context.h"
#include "mongo/db/admission/write_throttler_parameters_gen.h"
#include "mongo/db/client.h"
#include "mongo/db/server_options.h"
#include "mongo/util/decorable.h"
#include "mongo/util/tick_source.h"

#include <algorithm>

namespace mongo {

namespace {
const auto getWriteThrottlerDecoration =
    ServiceContext::declareDecoration<std::unique_ptr<WriteThrottler>>();

bool shouldInstallWriteThrottler() {
    return serverGlobalParams.clusterRole.has(ClusterRole::None) ||
        serverGlobalParams.clusterRole.has(ClusterRole::ShardServer);
}

const ConstructorActionRegistererType<ServiceContext> onServiceContextCreate{
    "InitWriteThrottler", [](ServiceContext* ctx) {
        if (shouldInstallWriteThrottler()) {
            getWriteThrottlerDecoration(ctx) = std::make_unique<WriteThrottler>();
        }
    }};
}  // namespace

WriteThrottler::WriteThrottler() : WriteThrottler(globalSystemTickSource()) {}

WriteThrottler::WriteThrottler(TickSource* tickSource)
    : _rateLimiter(std::make_unique<admission::RateLimiter>(static_cast<double>(kMaxRate),
                                                            gWriteThrottlerBurstCapacitySecs.load(),
                                                            gWriteThrottlerMaxQueueDepth.load(),
                                                            "writeThrottle",
                                                            tickSource)) {
    updateRate(gWriteThrottlerEnabled.load() ? gWriteThrottlerTargetRatePerSec.load() : kMaxRate);
}

void WriteThrottler::updateRate(int targetRatePerSec) {
    // Clamp to [1, kMaxRate]: values at or above kMaxRate mean "no throttle" and are normalized to
    // kMaxRate so the idle state is represented consistently in serverStatus/FTDC and the bucket is
    // not armed with a rate above kMaxRate.
    auto rate = std::max(1, std::min(targetRatePerSec, kMaxRate));
    _rateLimiter->updateRateParameters(static_cast<double>(rate),
                                       gWriteThrottlerBurstCapacitySecs.load());
    _rateLimiter->setMaxQueueDepth(gWriteThrottlerMaxQueueDepth.load());
}

void WriteThrottler::admitOperation(OperationContext* opCtx) {
    // Always forward to the rate limiter (like the always-on ingress rate limiter): when idle
    // (rate == kMaxRate) the token bucket admits immediately. The wait, if any, is attributed to
    // the WriteThrottlerAdmissionContext so curOp/serverStatus report the write-throttle queue.
    auto& admCtx = WriteThrottlerAdmissionContext::get(opCtx);
    {
        WaitingForAdmissionGuard admissionGuard(&admCtx,
                                                opCtx->getServiceContext()->getTickSource());
        uassertStatusOK(_rateLimiter->acquireToken(opCtx));
    }
    admCtx.recordAdmission();
}

void WriteThrottler::finalizeAdmission(OperationContext* opCtx) {
    auto& admCtx = WriteThrottlerAdmissionContext::get(opCtx);
    const int64_t admissions = admCtx.getAdmissions();
    if (admissions <= 0) {
        return;
    }

    const int64_t docs = admCtx.consumeWriteCostForReconciliation();
    if (docs <= 0) {
        return;
    }

    int64_t cost = docs;
    const int64_t maxCost = gWriteThrottlerMaxCostPerOp.load();
    if (maxCost > 0) {
        cost = std::min(cost, maxCost);
    }

    const int64_t extra = cost - admissions;
    if (extra > 0) {
        _rateLimiter->reconcileTokens(static_cast<double>(extra));
    }
}

void WriteThrottler::appendStats(BSONObjBuilder* bob) const {
    _rateLimiter->appendStats(bob);
}

BSONObj WriteThrottler::generateSection() const {
    BSONObjBuilder bob;
    bob.append("enabled", gWriteThrottlerEnabled.loadRelaxed());
    // Report the actual bucket refresh rate (source of truth) rather than the last cached value.
    bob.append("targetRateLimit", static_cast<int>(_rateLimiter->refreshRate()));
    appendStats(&bob);
    return bob.obj();
}

WriteThrottler* WriteThrottler::get(ServiceContext* service) {
    return getWriteThrottlerDecoration(service).get();
}

WriteThrottler* WriteThrottler::get(OperationContext* opCtx) {
    return get(opCtx->getServiceContext());
}

void WriteThrottler::set(ServiceContext* service, std::unique_ptr<WriteThrottler> throttler) {
    getWriteThrottlerDecoration(service) = std::move(throttler);
}

Status WriteThrottler::onUpdateTargetRatePerSec(int32_t targetRatePerSec) {
    // Inert when the throttler is off. When enabled, push the new target rate into the bucket.
    if (!gWriteThrottlerEnabled.load()) {
        return Status::OK();
    }
    if (auto client = Client::getCurrent()) {
        if (auto* t = WriteThrottler::get(client->getServiceContext())) {
            t->updateRate(targetRatePerSec);
        }
    }
    return Status::OK();
}

Status WriteThrottler::onUpdateEnabled(bool enabled) {
    if (auto client = Client::getCurrent()) {
        if (auto* t = WriteThrottler::get(client->getServiceContext())) {
            if (enabled) {
                // Push the current target rate into the bucket.
                t->updateRate(gWriteThrottlerTargetRatePerSec.load());
            } else {
                // Disarm: no throttling while the gate is off.
                t->updateRate(kMaxRate);
            }
        }
    }
    return Status::OK();
}

Status WriteThrottler::onUpdateBurstCapacitySecs(double burstCapacitySecs) {
    // Re-apply only the burst capacity, preserving the current effective rate. This mirrors
    // IngressRequestRateLimiter::onUpdateAdmissionBurstCapacitySecs and makes a runtime
    // setParameter take effect immediately rather than waiting for the next updateRate() call. It
    // is passive bucket configuration, so it is safe to apply while the throttler is off.
    if (auto client = Client::getCurrent()) {
        if (auto* t = WriteThrottler::get(client->getServiceContext())) {
            t->_rateLimiter->updateRateParameters(t->_rateLimiter->refreshRate(),
                                                  burstCapacitySecs);
        }
    }
    return Status::OK();
}

Status WriteThrottler::onUpdateMaxQueueDepth(long long maxQueueDepth) {
    // Re-apply only the queue depth, preserving the current rate. Mirrors
    // IngressRequestRateLimiter::onUpdateAdmissionMaxQueueDepth. This is passive bucket
    // configuration, so it is safe to apply while the throttler is off.
    if (auto client = Client::getCurrent()) {
        if (auto* t = WriteThrottler::get(client->getServiceContext())) {
            t->_rateLimiter->setMaxQueueDepth(maxQueueDepth);
        }
    }
    return Status::OK();
}

}  // namespace mongo
