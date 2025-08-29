/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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


#include "mongo/db/local_catalog/local_oplog_info.h"

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
// IWYU pragma: no_include "ext/alloc_traits.h"
#include "mongo/db/admission/flow_control.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/rss/replicated_storage_service.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/vector_clock/vector_clock_mutable.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/scopeguard.h"

#include <memory>
#include <mutex>
#include <utility>


namespace mongo {
namespace {

const auto oplogSlotTimeContext = OperationContext::declareDecoration<OplogSlotTimeContext>();
const auto localOplogInfo = ServiceContext::declareDecoration<LocalOplogInfo>();

}  // namespace

// static
LocalOplogInfo* LocalOplogInfo::get(ServiceContext& service) {
    return get(&service);
}

// static
LocalOplogInfo* LocalOplogInfo::get(ServiceContext* service) {
    return &localOplogInfo(service);
}

// static
LocalOplogInfo* LocalOplogInfo::get(OperationContext* opCtx) {
    return get(opCtx->getServiceContext());
}

// static
OplogSlotTimeContext& LocalOplogInfo::getOplogSlotTimeContext(OperationContext* opCtx) {
    return oplogSlotTimeContext(opCtx);
}

RecordStore* LocalOplogInfo::getRecordStore() const {
    stdx::lock_guard<stdx::mutex> lk(_rsMutex);
    return _rs;
}

void LocalOplogInfo::setRecordStore(OperationContext* opCtx, RecordStore* rs) {
    Timestamp lastAppliedOpTime;
    if (repl::feature_flags::gFeatureFlagOplogVisibility.isEnabled()) {
        lastAppliedOpTime =
            repl::ReplicationCoordinator::get(opCtx)->getMyLastAppliedOpTime().getTimestamp();
    }

    stdx::lock_guard<stdx::mutex> lk(_rsMutex);
    _rs = rs;
    // If the server was started in read-only mode or if we are restoring the node, skip
    // calculating the oplog truncate markers. The OplogCapMaintainerThread does not get started
    // in this instance.
    bool needsTruncateMarkers = opCtx->getServiceContext()->userWritesAllowed() &&
        !storageGlobalParams.repair && !repl::ReplSettings::shouldSkipOplogSampling();
    if (needsTruncateMarkers) {
        _truncateMarkers = OplogTruncateMarkers::createOplogTruncateMarkers(opCtx, *rs);
    }

    if (repl::feature_flags::gFeatureFlagOplogVisibility.isEnabled()) {
        _oplogVisibilityManager.reInit(_rs, lastAppliedOpTime);
    }
}

void LocalOplogInfo::resetRecordStore() {
    stdx::lock_guard<stdx::mutex> lk(_rsMutex);
    _rs = nullptr;

    if (repl::feature_flags::gFeatureFlagOplogVisibility.isEnabled()) {
        // It's possible for the oplog visibility manager to be uninitialized because
        // resetRecordStore may be called before setRecordStore in repair/standalone mode.
        _oplogVisibilityManager.clear();
    }
}

std::shared_ptr<OplogTruncateMarkers> LocalOplogInfo::getTruncateMarkers() const {
    stdx::lock_guard<stdx::mutex> lk(_rsMutex);
    return _truncateMarkers;
}

void LocalOplogInfo::setTruncateMarkers(std::shared_ptr<OplogTruncateMarkers> markers) {
    stdx::lock_guard<stdx::mutex> lk(_rsMutex);
    _truncateMarkers = std::move(markers);
}

void LocalOplogInfo::setNewTimestamp(ServiceContext* service, const Timestamp& newTime) {
    VectorClockMutable::get(service)->tickClusterTimeTo(LogicalTime(newTime));
}

std::vector<OplogSlot> LocalOplogInfo::getNextOpTimes(OperationContext* opCtx, std::size_t count) {
    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    long long term = repl::OpTime::kUninitializedTerm;

    // Fetch term out of the newOpMutex.
    if (replCoord->getSettings().isReplSet()) {
        // Current term. If we're not a replset of pv=1, it remains kOldProtocolVersionTerm.
        term = replCoord->getTerm();
    }

    Timestamp ts;
    // Provide a sample to FlowControl after the `oplogInfo.newOpMutex` is released.
    ON_BLOCK_EXIT([opCtx, &ts, count] {
        auto& rss = rss::ReplicatedStorageService::get(opCtx);
        if (!rss.getPersistenceProvider().shouldUseOplogWritesForFlowControlSampling())
            return;

        auto flowControl = FlowControl::get(opCtx);
        if (flowControl) {
            flowControl->sample(ts, count);
        }
    });

    auto storageEngine = opCtx->getServiceContext()->getStorageEngine();

    const bool isFirstOpTime = !shard_role_details::getRecoveryUnit(opCtx)->isTimestamped();

    // Allow the storage engine to start the transaction outside the critical section.
    shard_role_details::getRecoveryUnit(opCtx)->preallocateSnapshot();
    {
        stdx::lock_guard<stdx::mutex> lk(_newOpMutex);

        ts = VectorClockMutable::get(opCtx)->tickClusterTime(count).asTimestamp();
        const bool orderedCommit = false;

        // The local oplog record store pointer must already be established by this point.
        // We can't establish it here because that would require locking the local database, which
        // would be a lock order violation.
        invariant(_rs);
        fassert(28560, storageEngine->oplogDiskLocRegister(opCtx, _rs, ts, orderedCommit));
    }

    const auto prevAssertOnLockAttempt =
        shard_role_details::getLocker(opCtx)->getAssertOnLockAttempt();
    const auto prevRuBlockingAllowed =
        shard_role_details::getRecoveryUnit(opCtx)->getBlockingAllowed();
    if (isFirstOpTime) {
        // This transaction has begun holding a resource in the form of an oplog slot. Committed
        // transactions that get a later oplog slot will be unable to replicate until this resource
        // is released (in the form of this transaction committing or aborting). We choose to fail
        // the operation if it blocks on a lock or a storage engine operation, like a prepared
        // update. Both scenarios could stall the system and stop replication.
        shard_role_details::getLocker(opCtx)->setAssertOnLockAttempt(true);
        shard_role_details::getRecoveryUnit(opCtx)->setBlockingAllowed(false);
    }
    oplogSlotTimeContext(opCtx).incBatchCount();
    std::vector<OplogSlot> oplogSlots(count);
    for (std::size_t i = 0; i < count; i++) {
        oplogSlots[i] = {Timestamp(ts.asULL() + i), term};
    }

    // If we abort a transaction that has reserved an optime, we should make sure to update the
    // stable timestamp if necessary, since this oplog hole may have been holding back the stable
    // timestamp.
    shard_role_details::getRecoveryUnit(opCtx)->onRollback([replCoord,
                                                            isFirstOpTime,
                                                            prevAssertOnLockAttempt,
                                                            prevRuBlockingAllowed](
                                                               OperationContext* opCtx) {
        replCoord->attemptToAdvanceStableTimestamp();
        oplogSlotTimeContext(opCtx).decBatchCount();

        // Only reset these properties when the first slot is released.
        if (isFirstOpTime) {
            shard_role_details::getLocker(opCtx)->setAssertOnLockAttempt(prevAssertOnLockAttempt);
            shard_role_details::getRecoveryUnit(opCtx)->setBlockingAllowed(prevRuBlockingAllowed);
        }
    });

    shard_role_details::getRecoveryUnit(opCtx)->onCommit([isFirstOpTime,
                                                          prevAssertOnLockAttempt,
                                                          prevRuBlockingAllowed](
                                                             OperationContext* opCtx,
                                                             boost::optional<Timestamp>) {
        oplogSlotTimeContext(opCtx).decBatchCount();

        // Only reset these properties when the first slot is released.
        if (isFirstOpTime) {
            shard_role_details::getLocker(opCtx)->setAssertOnLockAttempt(prevAssertOnLockAttempt);
            shard_role_details::getRecoveryUnit(opCtx)->setBlockingAllowed(prevRuBlockingAllowed);
        }
    });

    return oplogSlots;
}

}  // namespace mongo
