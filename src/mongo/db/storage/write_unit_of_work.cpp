/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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


#include <ostream>
#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>

#include "mongo/bson/timestamp.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/time_support.h"

namespace mongo {

MONGO_FAIL_POINT_DEFINE(sleepBeforeCommit);

WriteUnitOfWork::WriteUnitOfWork(OperationContext* opCtx, OplogEntryGroupType groupOplogEntries)
    : _opCtx(opCtx),
      _toplevel(opCtx->_ruState == RecoveryUnitState::kNotInUnitOfWork),
      _groupOplogEntries(groupOplogEntries) {  // Grouping oplog entries doesn't support WUOW
                                               // nesting (e.g. multi-doc transactions).
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
        if (!_opCtx->readOnly()) {
            if (_toplevel) {
                // Abort unit of work and execute rollback handlers
                shard_role_details::getRecoveryUnit(_opCtx)->abortUnitOfWork();
                _opCtx->_ruState = RecoveryUnitState::kNotInUnitOfWork;
            } else {
                _opCtx->_ruState = RecoveryUnitState::kFailedUnitOfWork;
            }
        } else {
            // Clear the readOnly state and execute rollback handlers in readOnly mode.
            shard_role_details::getRecoveryUnit(_opCtx)->endReadOnlyUnitOfWork();
            shard_role_details::getRecoveryUnit(_opCtx)->abortRegisteredChanges();
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
        if (MONGO_unlikely(sleepBeforeCommit.shouldFail())) {
            sleepFor(Milliseconds(100));
        }

        // Execute preCommit hooks before committing the transaction. This is an opportunity to
        // throw or do any last changes before committing.
        shard_role_details::getRecoveryUnit(_opCtx)->runPreCommitHooks(_opCtx);
        if (!_opCtx->readOnly()) {
            // Commit unit of work and execute commit or rollback handlers depending on whether the
            // commit was successful.
            shard_role_details::getRecoveryUnit(_opCtx)->commitUnitOfWork();
        } else {
            // Just execute commit handlers in readOnly mode
            shard_role_details::getRecoveryUnit(_opCtx)->commitRegisteredChanges(boost::none);
        }

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
