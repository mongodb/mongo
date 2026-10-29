// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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

// Bounds a single acquireToken() wait when writeThrottlerMaxCostPerOp is unlimited, so one large
// admission cannot queue for an unbounded token request size.
constexpr int64_t kMaxAdmissionTokenChunk = 1024;

void recordAdmissions(WriteThrottlerAdmissionContext& admCtx, int64_t admissions) {
    for (int64_t i = 0; i < admissions; ++i) {
        admCtx.recordAdmission();
    }
}

int64_t capRemainingCost(WriteThrottlerAdmissionContext& admCtx, int64_t cost) {
    if (cost <= 0) {
        return 0;
    }

    const int64_t maxCost = gWriteThrottlerMaxCostPerOp.load();
    if (maxCost <= 0) {
        return cost;
    }

    const int64_t admissions = admCtx.getAdmissions();
    if (admissions >= maxCost) {
        return 0;
    }
    return std::min(cost, maxCost - admissions);
}

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
    _rateLimiter->updateRateParametersPreservingBalance(static_cast<double>(rate),
                                                        gWriteThrottlerBurstCapacitySecs.load());
    _rateLimiter->setMaxQueueDepth(gWriteThrottlerMaxQueueDepth.load());
}

void WriteThrottler::admitOperation(OperationContext* opCtx, int64_t cost) {
    // Always forward to the rate limiter (like the always-on ingress rate limiter): when idle
    // (rate == kMaxRate) the token bucket admits immediately. The wait, if any, is attributed to
    // the WriteThrottlerAdmissionContext so curOp/serverStatus report the write-throttle queue.
    auto& admCtx = WriteThrottlerAdmissionContext::get(opCtx);
    const bool serviceEntryAdmission = admCtx.getAdmissions() == 0 && cost == 1;
    cost = capRemainingCost(admCtx, cost);
    if (cost <= 0) {
        return;
    }

    int64_t remaining = cost;
    while (remaining > 0) {
        const int64_t chunk = std::min(remaining, kMaxAdmissionTokenChunk);
        uassertStatusOK(_rateLimiter->acquireToken(opCtx, &admCtx, static_cast<double>(chunk)));
        remaining -= chunk;
    }

    recordAdmissions(admCtx, cost);
    if (serviceEntryAdmission) {
        admCtx.markServiceEntryAdmissionCredit();
    }
}

void WriteThrottler::admitKnownWrites(OperationContext* opCtx, int64_t knownWrites) {
    auto& admCtx = WriteThrottlerAdmissionContext::get(opCtx);
    if (knownWrites <= 0 || admCtx.getAdmissions() <= 0) {
        return;
    }

    if (admCtx.consumeServiceEntryAdmissionCredit()) {
        --knownWrites;
    }
    admitOperation(opCtx, knownWrites);
}

void WriteThrottler::finalizeAdmission(OperationContext* opCtx) {
    auto& admCtx = WriteThrottlerAdmissionContext::get(opCtx);
    const int64_t admissions = admCtx.getAdmissions();
    if (admissions <= 0) {
        return;
    }

    // True-up against storage-engine key writes (document records + index keys) recorded at the WT
    // cursor helpers. Known writes already charged through admissions are reconciled here.
    const int64_t storageWrites = admCtx.getStorageWrites();
    int64_t cost = storageWrites;
    const int64_t maxCost = gWriteThrottlerMaxCostPerOp.load();
    if (maxCost > 0) {
        cost = std::min(cost, maxCost);
    }

    // Signed true-up: positive extra drains/borrows; negative returns surplus tokens.
    _rateLimiter->reconcileTokens(static_cast<double>(cost - admissions));
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
            t->_rateLimiter->updateRateParametersPreservingBalance(t->_rateLimiter->refreshRate(),
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
