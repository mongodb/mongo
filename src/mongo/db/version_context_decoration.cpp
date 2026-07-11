// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/operation_context.h"
#include "mongo/db/version_context.h"

// Override waitForOperationsNotMatchingVersionContextToComplete's internal re-check interval
MONGO_FAIL_POINT_DEFINE(waitBeforeFixedOperationFCVRegionRaceCheck);
MONGO_FAIL_POINT_DEFINE(reduceWaitForOfcvInternalIntervalTo10Ms);
#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo {

void VersionContext::setDecoration(ClientLock&,
                                   OperationContext* opCtx,
                                   const VersionContext& vCtx) {
    tassert(9955801, "Expected incoming versionContext to have an OFCV", vCtx.hasOperationFCV());

    // We disallow setting a VersionContext decoration multiple times on the same OperationContext,
    // even if it's the same one. There is no use case for it, and makes our implementation more
    // complex (e.g. ScopedSetDecoration would need to have a "no-op" destructor path).
    auto& originalVCtx = _getDecoration(opCtx);
    tassert(10296500,
            "Refusing to set a VersionContext on an operation that already has one",
            !originalVCtx.hasOperationFCV());
    originalVCtx = vCtx;
}

void VersionContext::setFromMetadata(ClientLock& lk,
                                     OperationContext* opCtx,
                                     const VersionContext& vCtx) {
    VersionContext::setDecoration(lk, opCtx, vCtx);
}

VersionContext::ScopedSetDecoration::ScopedSetDecoration(OperationContext* opCtx,
                                                         const VersionContext& vCtx)
    : _opCtx(opCtx) {
    ClientLock lk(opCtx->getClient());
    VersionContext::setDecoration(lk, opCtx, vCtx);
}

VersionContext::ScopedSetDecoration::~ScopedSetDecoration() {
    ClientLock lk(_opCtx->getClient());
    _getDecoration(_opCtx).resetToOperationWithoutOFCV();
}

VersionContext::FixedOperationFCVRegion::FixedOperationFCVRegion(OperationContext* opCtx)
    : _opCtx(opCtx) {
    if (_getDecoration(_opCtx).hasOperationFCV()) {
        // Already has a VersionContext (re-entrancy)
        _opCtx = nullptr;
        return;
    }

    auto getFCV = []() -> auto {
        auto fcvSnapshot = serverGlobalParams.featureCompatibility.acquireFCVSnapshot();
        return fcvSnapshot.isVersionInitialized()
            ? fcvSnapshot.getVersion()
            : multiversion::FeatureCompatibilityVersion::kUnsetDefaultLastLTSBehavior;
    };

    ClientLock lk(_opCtx->getClient());

    // Prevent operations acquiring the OFCV to outlive FCV transitions: without this, an operation
    // that starts on a primary node may continue after stepping down and up again (the OFCV
    // draining happens only on primary nodes during setFCV).
    _opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

    while (true) {
        auto fcv = getFCV();
        VersionContext::setDecoration(lk, _opCtx, VersionContext(fcv));

        waitBeforeFixedOperationFCVRegionRaceCheck.pauseWhileSet();

        if (fcv == getFCV()) {
            // Snapshot acquisition did not race with FCV transition, it is then ensured
            // that the operation will be properly drained upon setFCV if needed.
            break;
        }

        // Iterate again to install the current OFCV because the snapshot acquisition raced with a
        // FCV transition
        _getDecoration(_opCtx).resetToOperationWithoutOFCV();
    }
}

VersionContext::FixedOperationFCVRegion::~FixedOperationFCVRegion() {
    if (_opCtx == nullptr) {
        return;
    }

    ClientLock lk(_opCtx->getClient());
    _getDecoration(_opCtx).resetToOperationWithoutOFCV();
}

void waitForOperationsNotMatchingVersionContextToComplete(OperationContext* opCtx,
                                                          const VersionContext& expectedVCtx,
                                                          const Date_t deadline) {
    auto serviceCtx = opCtx->getServiceContext();
    invariant(serviceCtx);

    auto getOperationsDrainingFutures = [&](bool firstTime) {
        std::vector<SemiFuture<void>> futures;
        for (ServiceContext::LockedClientsCursor cursor(serviceCtx);
             Client* client = cursor.next();) {
            ClientLock lk(client);

            OperationContext* clientOpCtx = client->getOperationContext();
            if (!clientOpCtx || clientOpCtx->isKillPending()) {
                continue;
            }

            const auto& actualVCtx = VersionContext::getDecoration(clientOpCtx);
            if (actualVCtx.hasOperationFCV() && actualVCtx != expectedVCtx) {
                if (firstTime) {
                    LOGV2_DEBUG(11144500,
                                1,
                                "Operation running under stale FCV detected, adding to the list of "
                                "operations to drain",
                                "opId"_attr = clientOpCtx->getOpID(),
                                "operationFcv"_attr = actualVCtx.toBSON(),
                                "currentFcv"_attr = expectedVCtx.toBSON());
                } else {
                    LOGV2_DEBUG(11144501,
                                3,
                                "Still waiting for operation running under stale FCV to drain",
                                "opId"_attr = clientOpCtx->getOpID(),
                                "operationFcv"_attr = actualVCtx.toBSON(),
                                "currentFcv"_attr = expectedVCtx.toBSON());
                }
                futures.emplace_back(clientOpCtx->getCancellationToken().onCancel());
            }
        }
        return futures;
    };

    auto operationsDrainingFutures = getOperationsDrainingFutures(true /* firstTime */);
    const int initialNumOpsWithStaleOfcv = operationsDrainingFutures.size();

    LOGV2_DEBUG(11144502,
                1,
                "Starting waiting for operations running under stale FCV to drain",
                "numOpsToDrain"_attr = initialNumOpsWithStaleOfcv,
                "currentFcv"_attr = expectedVCtx.toBSON());

    // This draining logic waits for the entire OperationContext to finish, rather than for the OFCV
    // to be released. This may be problematic in case of long running operations just using the
    // OFCV for a small duration.
    // If draining takes more than 10 seconds, we re-scan the operation contexts to determine which
    // ones should be still waited on.
    while (operationsDrainingFutures.size() > 0) {
        const auto kRecheckOFCVInterval =
            MONGO_unlikely(reduceWaitForOfcvInternalIntervalTo10Ms.shouldFail())
            ? Milliseconds(10)
            : Milliseconds(10000);

        const auto waitForInterval = std::min(deadline, Date_t::now() + kRecheckOFCVInterval);

        try {
            for (const auto& future : operationsDrainingFutures) {
                opCtx->runWithDeadline(waitForInterval, ErrorCodes::ExceededTimeLimit, [&] {
                    try {
                        future.get(opCtx);
                    } catch (const ExceptionFor<ErrorCodes::CallbackCanceled>&) {
                        // The cancellation token's error is set to ErrorCodes::CallbackCanceled
                        // when an operation successfully drains
                    }
                });
            }

            // All futures were successfully waited on, no need to loop again
            break;
        } catch (const ExceptionFor<ErrorCodes::ExceededTimeLimit>&) {
            if (deadline < Date_t::now()) {
                LOGV2_DEBUG(11144503,
                            1,
                            "Reached deadline while waiting for operations running under stale FCV "
                            "to drain",
                            "numOpsToDrain"_attr = initialNumOpsWithStaleOfcv,
                            "currentFcv"_attr = expectedVCtx.toBSON(),
                            "deadline"_attr = deadline);
                throw;
            }

            // The deadline is in the future, re-compute list of futures to wait on and loop again
            operationsDrainingFutures = getOperationsDrainingFutures(false /* firstTime */);
        }
    }

    LOGV2_DEBUG(11144504,
                1,
                "Finished waiting for operations running under stale FCV to drain",
                "numOpsToDrain"_attr = initialNumOpsWithStaleOfcv,
                "currentFcv"_attr = expectedVCtx.toBSON());
}

}  // namespace mongo
