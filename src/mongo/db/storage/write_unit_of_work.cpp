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

#include "mongo/platform/basic.h"

#include "mongo/db/storage/write_unit_of_work.h"

#include "mongo/db/operation_context.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/time_support.h"

namespace mongo {

MONGO_FAIL_POINT_DEFINE(sleepBeforeCommit);

WriteUnitOfWork::WriteUnitOfWork(OperationContext* opCtx)
    : _opCtx(opCtx), _toplevel(opCtx->_ruState == RecoveryUnitState::kNotInUnitOfWork) {
    uassert(ErrorCodes::IllegalOperation,
            "Cannot execute a write operation in read-only mode",
            !storageGlobalParams.readOnly);
    _opCtx->lockState()->beginWriteUnitOfWork();
    if (_toplevel) {
        _opCtx->recoveryUnit()->beginUnitOfWork(_opCtx);
        _opCtx->_ruState = RecoveryUnitState::kActiveUnitOfWork;
    }
}

WriteUnitOfWork::~WriteUnitOfWork() {
    dassert(!storageGlobalParams.readOnly);
    if (!_released && !_committed) {
        invariant(_opCtx->_ruState != RecoveryUnitState::kNotInUnitOfWork);
        if (_toplevel) {
            _opCtx->recoveryUnit()->abortUnitOfWork();
            _opCtx->_ruState = RecoveryUnitState::kNotInUnitOfWork;
        } else {
            _opCtx->_ruState = RecoveryUnitState::kFailedUnitOfWork;
        }
        _opCtx->lockState()->endWriteUnitOfWork();
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

    _opCtx->recoveryUnit()->prepareUnitOfWork();
    _prepared = true;
}

void WriteUnitOfWork::commit() {
    invariant(!_committed);
    invariant(!_released);
    invariant(_opCtx->_ruState == RecoveryUnitState::kActiveUnitOfWork);
    if (_toplevel) {
        if (MONGO_FAIL_POINT(sleepBeforeCommit)) {
            sleepFor(Milliseconds(100));
        }

        _opCtx->recoveryUnit()->commitUnitOfWork();
        _opCtx->_ruState = RecoveryUnitState::kNotInUnitOfWork;
    }
    _opCtx->lockState()->endWriteUnitOfWork();
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
