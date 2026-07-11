// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/future.h"
#include "mongo/util/modules.h"
#include "mongo/util/time_support.h"

#include <cstdint>
#include <memory>

#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::insert_listener {

// An abstract class used to notify on new insert events.
class Notifier {
public:
    virtual ~Notifier() {};

    // Performs the necessary work needed for waiting. Should be called prior calling waitUntil().
    virtual void prepareForWait(OperationContext* opCtx) = 0;

    // Performs any necessary steps after waiting. Should be called after waitUntil().
    // After calling doneWaiting, the caller must attempt to read the data waited for before
    // calling prepareForWait and waitUntil again, or a spurious wait may occur.
    virtual void doneWaiting(OperationContext* opCtx) = 0;

    // Blocks the caller until an insert event is fired or the deadline is hit.  Must be robust
    // to being called multiple times without an intervening read.
    virtual void waitUntil(OperationContext* opCtx, Date_t deadline) = 0;
};

// Class used to notify listeners of local inserts into the capped collection.
class LocalCappedInsertNotifier final : public Notifier {
public:
    LocalCappedInsertNotifier(std::shared_ptr<CappedInsertNotifier> notifier)
        : _notifier(std::move(notifier)) {
        tassert(11321505, "notifier must not be null", _notifier);
    }

    void prepareForWait(OperationContext* opCtx) final {
        _currentVersion = _notifier->getVersion();
    }

    void waitUntil(OperationContext* opCtx, Date_t deadline) final {
        _notifier->waitUntil(opCtx, _lastEOFVersion, deadline);
    }

    void doneWaiting(OperationContext* opCtx) final {
        _lastEOFVersion = _currentVersion;
    }

private:
    std::shared_ptr<CappedInsertNotifier> _notifier;
    uint64_t _lastEOFVersion = ~uint64_t(0);
    // This will be initialized by prepareForWait.
    uint64_t _currentVersion;
};

// Class used to notify listeners on majority committed point advancement events.
class MajorityCommittedPointNotifier final : public Notifier {
public:
    MajorityCommittedPointNotifier(repl::OpTime opTime = repl::OpTime())
        : _opTimeToBeMajorityCommitted(opTime) {}

    // Computes the OpTime to wait on by incrementing the current read timestamp.
    void prepareForWait(OperationContext* opCtx) final {
        auto readTs = shard_role_details::getRecoveryUnit(opCtx)->getPointInTimeReadTimestamp();
        tassert(11321506, "readTs must not be none", readTs);
        _opTimeToBeMajorityCommitted =
            repl::OpTime(*readTs + 1, repl::ReplicationCoordinator::get(opCtx)->getTerm());
    }

    void waitUntil(OperationContext* opCtx, Date_t deadline) final {
        auto majorityCommittedFuture = WaitForMajorityService::get(opCtx->getServiceContext())
                                           .waitUntilMajorityForRead(_opTimeToBeMajorityCommitted,
                                                                     opCtx->getCancellationToken());
        opCtx->runWithDeadline(deadline, opCtx->getTimeoutError(), [&] {
            auto status = majorityCommittedFuture.getNoThrow(opCtx);
            if (!status.isOK()) {
                LOGV2_DEBUG(7455500,
                            3,
                            "Failure waiting for the majority committed event",
                            "status"_attr = status.toString());
            }
        });
    }

    void doneWaiting(OperationContext* opCtx) final {}

private:
    repl::OpTime _opTimeToBeMajorityCommitted;
};

/**
 * Returns true if the PlanExecutor should listen for inserts, which is when a getMore is called
 * on a tailable and awaitData cursor that still has time left and hasn't been interrupted.
 */

bool shouldListenForInserts(OperationContext* opCtx, CanonicalQuery* cq);

/**
 * Returns true if the PlanExecutor should wait for data to be inserted, which is when a getMore
 * is called on a tailable and awaitData cursor on a capped collection.  Returns false if an EOF
 * should be returned immediately.
 */
bool shouldWaitForInserts(OperationContext* opCtx,
                          CanonicalQuery* cq,
                          PlanYieldPolicy* yieldPolicy);

/**
 * Returns an insert notifier for a capped collection.
 */
std::unique_ptr<Notifier> getCappedInsertNotifier(
    OperationContext* opCtx,
    const boost::optional<CollectionAcquisition>& collection,
    PlanYieldPolicy* yieldPolicy);

/**
 * Called for tailable and awaitData cursors in order to yield locks and waits for inserts to
 * the collection being tailed. Returns control to the caller once there has been an insertion
 * and there may be new results. If the PlanExecutor was killed during a yield, throws an
 * exception.
 */
void waitForInserts(OperationContext* opCtx,
                    PlanYieldPolicy* yieldPolicy,
                    std::unique_ptr<Notifier>& notifier);
}  // namespace mongo::insert_listener

#undef MONGO_LOGV2_DEFAULT_COMPONENT
