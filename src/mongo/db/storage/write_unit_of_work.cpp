// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/storage/write_unit_of_work.h"

#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/util/assert_util.h"

#include <ostream>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>

namespace mongo {
namespace {

const auto getOplogGroupingPolicy =
    ServiceContext::declareDecoration<std::unique_ptr<OplogGroupingPolicy>>();

/**
 * Returns the grouping type to be used, converting kDontGroup to a batched mode for a top-level
 * WriteUnitOfWork if the operation is not a multi-document transaction and oplog entry grouping is
 * enabled. The chosen mode depends on whether the write is retryable:
 *   - retryable write -> kGroupForAtomicWrite (single retryable statement, applied atomically).
 *   - otherwise       -> kGroupForTransaction (atomic batched write without session info).
 *
 * A caller that requests an explicit (non-kDontGroup) grouping mode bypasses this conversion:
 * 'groupType' is already set, so it is returned unchanged. This is how a write that intentionally
 * batches multiple statements opts out of the single-statement atomic mode.
 */
WriteUnitOfWork::OplogEntryGroupType getGroupType(OperationContext* opCtx,
                                                  WriteUnitOfWork::OplogEntryGroupType groupType,
                                                  bool topLevel) {
    if (opCtx->inMultiDocumentTransaction() ||
        groupType != WriteUnitOfWork::OplogEntryGroupType::kDontGroup || !topLevel) {
        return groupType;
    }

    if (!OplogGroupingPolicy::get(opCtx->getServiceContext()).shouldGroupOplogEntries(opCtx)) {
        return groupType;
    }

    // Multi-document transactions already returned above, so a present txnNumber here means a
    // retryable write.
    return opCtx->getTxnNumber() ? WriteUnitOfWork::OplogEntryGroupType::kGroupForAtomicWrite
                                 : WriteUnitOfWork::OplogEntryGroupType::kGroupForTransaction;
}

}  // namespace

OplogGroupingPolicy& OplogGroupingPolicy::get(ServiceContext* svc) {
    auto& policy = getOplogGroupingPolicy(svc);
    if (policy) {
        return *policy;
    }
    static OplogGroupingPolicy defaultPolicy;
    return defaultPolicy;
}

void OplogGroupingPolicy::set(ServiceContext* svc, std::unique_ptr<OplogGroupingPolicy> policy) {
    getOplogGroupingPolicy(svc) = std::move(policy);
}

WriteUnitOfWork::WriteUnitOfWork(OperationContext* opCtx, OplogEntryGroupType groupOplogEntries)
    : _opCtx(opCtx),
      _toplevel(opCtx->_ruState == RecoveryUnitState::kNotInUnitOfWork),
      _groupOplogEntries(getGroupType(opCtx, groupOplogEntries, _toplevel)) {
    // Grouping oplog entries doesn't support WUOW nesting (e.g. multi-doc transactions).
    invariant(_toplevel || !_isGroupingOplogEntries());

    if (_isGroupingOplogEntries()) {
        const auto opObserver = _opCtx->getServiceContext()->getOpObserver();
        invariant(opObserver);
        opObserver->onBatchedWriteStart(_opCtx);
    }

    shard_role_details::getLocker(_opCtx)->beginWriteUnitOfWork();
    if (_toplevel) {
        shard_role_details::getRecoveryUnit(_opCtx)->beginUnitOfWork(_opCtx->readOnly());
        _opCtx->_ruState = RecoveryUnitState::kActiveUnitOfWork;
    }

    // Make sure we don't silently proceed after a previous WriteUnitOfWork under the same parent
    // WriteUnitOfWork fails.
    invariant(_opCtx->_ruState != RecoveryUnitState::kFailedUnitOfWork);
}

WriteUnitOfWork::~WriteUnitOfWork() {
    if (!_released && !_committed) {
        invariant(_opCtx->_ruState != RecoveryUnitState::kNotInUnitOfWork);
        if (_toplevel) {
            // Abort unit of work and execute rollback handlers
            shard_role_details::getRecoveryUnit(_opCtx)->abortUnitOfWork();
            _opCtx->_ruState = RecoveryUnitState::kNotInUnitOfWork;
        } else {
            _opCtx->_ruState = RecoveryUnitState::kFailedUnitOfWork;
        }
        shard_role_details::getLocker(_opCtx)->endWriteUnitOfWork();
    }

    if (_isGroupingOplogEntries()) {
        const auto opObserver = _opCtx->getServiceContext()->getOpObserver();
        invariant(opObserver);
        opObserver->onBatchedWriteAbort(_opCtx);
    }
}

std::unique_ptr<WriteUnitOfWork> WriteUnitOfWork::createForSnapshotResume(
    OperationContext* opCtx, RecoveryUnitState ruState) {
    auto wuow = std::unique_ptr<WriteUnitOfWork>(new WriteUnitOfWork());
    wuow->_opCtx = opCtx;
    wuow->_toplevel = true;
    wuow->_opCtx->_ruState = ruState;
    return wuow;
}

WriteUnitOfWork::RecoveryUnitState WriteUnitOfWork::release() {
    auto ruState = _opCtx->_ruState;
    invariant(ruState == RecoveryUnitState::kActiveUnitOfWork ||
              ruState == RecoveryUnitState::kFailedUnitOfWork);
    invariant(!_committed);
    invariant(_toplevel);

    _released = true;
    _opCtx->_ruState = RecoveryUnitState::kNotInUnitOfWork;
    return ruState;
}

void WriteUnitOfWork::prepare() {
    invariant(!_committed);
    invariant(!_prepared);
    invariant(_toplevel);
    invariant(_opCtx->_ruState == RecoveryUnitState::kActiveUnitOfWork);

    shard_role_details::getRecoveryUnit(_opCtx)->prepareUnitOfWork();
    _prepared = true;
}

void WriteUnitOfWork::commit() {
    invariant(!_committed);
    invariant(!_released);
    invariant(_opCtx->_ruState == RecoveryUnitState::kActiveUnitOfWork);

    if (_isGroupingOplogEntries()) {
        const auto opObserver = _opCtx->getServiceContext()->getOpObserver();
        invariant(opObserver);
        opObserver->onBatchedWriteCommit(_opCtx, _groupOplogEntries);
    }
    if (_toplevel) {
        shard_role_details::getRecoveryUnit(_opCtx)->commitUnitOfWork();
        _opCtx->_ruState = RecoveryUnitState::kNotInUnitOfWork;
    }
    shard_role_details::getLocker(_opCtx)->endWriteUnitOfWork();
    _committed = true;
}

std::ostream& operator<<(std::ostream& os, WriteUnitOfWork::RecoveryUnitState state) {
    switch (state) {
        case WriteUnitOfWork::kNotInUnitOfWork:
            return os << "NotInUnitOfWork";
        case WriteUnitOfWork::kActiveUnitOfWork:
            return os << "ActiveUnitOfWork";
        case WriteUnitOfWork::kFailedUnitOfWork:
            return os << "FailedUnitOfWork";
    }
    MONGO_UNREACHABLE;
}

}  // namespace mongo
