// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/sbe/stages/shared_read_view.h"

#include "mongo/db/client.h"
#include "mongo/db/shard_role/shard_catalog/allow_read_from_latest_on_secondary.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/util/assert_util.h"

#include <mutex>

namespace mongo::sbe {

namespace {

/**
 * Whether the reads of 'opCtx' can be carried out by other OperationContexts given its view, and
 * if not, why.
 */
Status checkShareable(OperationContext* opCtx) {
    auto* ru = shard_role_details::getRecoveryUnit(opCtx);
    // The writes and the snapshot of a multi-document transaction or of a unit of work (whether
    // opened by a WriteUnitOfWork or directly on the RecoveryUnit) live in the operation's own
    // storage session, which no other session can see.
    if (opCtx->inMultiDocumentTransaction()) {
        return {ErrorCodes::IllegalOperation, "multi-document transaction"};
    }
    if (ru->inUnitOfWork()) {
        return {ErrorCodes::IllegalOperation, "unit of work in progress"};
    }

    // Linearizable reads are only guaranteed for single-document reads served from one snapshot.
    if (repl::ReadConcernArgs::get(opCtx).getLevel() ==
        repl::ReadConcernLevel::kLinearizableReadConcern) {
        return {ErrorCodes::IllegalOperation, "linearizable read concern"};
    }

    // An operation allowed to read the latest data on a secondary reads without a timestamp, but
    // the acquisitions of the workers, which aren't allowed to, would read at lastApplied.
    if (allowReadFromLatestOnSecondary(opCtx)) {
        return {ErrorCodes::IllegalOperation, "reads the latest data on a secondary"};
    }

    // A pinned read source must not be changed by the acquisitions of the workers, which would
    // re-derive it from the replication state.
    if (ru->isReadSourcePinned()) {
        return {ErrorCodes::IllegalOperation, "pinned read source"};
    }

    switch (ru->getTimestampReadSource()) {
        case RecoveryUnit::ReadSource::kNoTimestamp:
        case RecoveryUnit::ReadSource::kLastApplied:
        case RecoveryUnit::ReadSource::kMajorityCommitted:
        case RecoveryUnit::ReadSource::kProvided:
            return Status::OK();
        // kNoOverlap backs speculative majority reads, which are no longer used in the codebase.
        // kAllDurableSnapshot is resolved once, at the first snapshot of the operation, and is
        // only used by multi-document transactions.
        case RecoveryUnit::ReadSource::kNoOverlap:
            return {ErrorCodes::IllegalOperation, "kNoOverlap read source"};
        case RecoveryUnit::ReadSource::kAllDurableSnapshot:
            return {ErrorCodes::IllegalOperation, "kAllDurableSnapshot read source"};
    }
    MONGO_UNREACHABLE;
}

}  // namespace


StatusWith<SharedReadView> SharedReadView::capture(OperationContext* opCtx) {
    if (auto status = checkShareable(opCtx); !status.isOK()) {
        return status;
    }

    auto* ru = shard_role_details::getRecoveryUnit(opCtx);
    SharedReadView view;
    view._readConcern = repl::ReadConcernArgs::get(opCtx);
    switch (ru->getTimestampReadSource()) {
        case RecoveryUnit::ReadSource::kProvided:
            view._readSource = RecoveryUnit::ReadSource::kProvided;
            view._readTimestamp = ru->getPointInTimeReadTimestamp();
            break;
        case RecoveryUnit::ReadSource::kMajorityCommitted:
            view._readSource = RecoveryUnit::ReadSource::kMajorityCommitted;
            break;
        default:
            // kLastApplied isn't chosen by the operation: each acquisition picks it, or
            // kNoTimestamp, from the read concern and the current replication state, and the
            // RecoveryUnit resets it at every snapshot close. So the worker starts from
            // kNoTimestamp and its own acquisitions make the same choice the main operation makes
            // when it restores after a yield.
            view._readSource = RecoveryUnit::ReadSource::kNoTimestamp;
            break;
    }
    view._prepareConflictBehavior = ru->getPrepareConflictBehavior();
    view._readOnce = ru->getReadOnce();
    view._enforceConstraints = opCtx->isEnforcingConstraints();
    view._deadline = opCtx->getDeadline();
    view._timeoutError = opCtx->getTimeoutError();
    return view;
}

void SharedReadView::applyTo(OperationContext* workerOpCtx) const {
    auto* ru = shard_role_details::getRecoveryUnit(workerOpCtx);
    tassert(13436701,
            "A shared read view must be applied before the worker opens a snapshot",
            !ru->isActive());
    tassert(13436702,
            "A shared read view must be applied before the worker acquires any collection",
            !shard_role_details::TransactionResources::isPresent(workerOpCtx) ||
                shard_role_details::TransactionResources::get(workerOpCtx).isEmpty());

    {
        // CurrentOp may read the ReadConcernArgs of the worker concurrently.
        std::lock_guard<Client> lk(*workerOpCtx->getClient());
        repl::ReadConcernArgs::get(workerOpCtx) = _readConcern;
    }
    ru->setTimestampReadSource(_readSource, _readTimestamp);
    ru->setPrepareConflictBehavior(_prepareConflictBehavior);
    ru->setReadOnce(_readOnce);
    workerOpCtx->setEnforceConstraints(_enforceConstraints);
    if (_deadline != Date_t::max()) {
        workerOpCtx->setDeadlineByDate(_deadline, _timeoutError);
    }
}

}  // namespace mongo::sbe
