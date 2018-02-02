/**
 *    Copyright (C) 2018 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/storage/write_unit_of_work.h"

namespace mongo {

WriteUnitOfWork::WriteUnitOfWork(OperationContext* opCtx)
    : _opCtx(opCtx), _toplevel(opCtx->_ruState == OperationContext::kNotInUnitOfWork) {
    uassert(ErrorCodes::IllegalOperation,
            "Cannot execute a write operation in read-only mode",
            !storageGlobalParams.readOnly);
    _opCtx->lockState()->beginWriteUnitOfWork();
    if (_toplevel) {
        _opCtx->recoveryUnit()->beginUnitOfWork(_opCtx);
        _opCtx->_ruState = OperationContext::kActiveUnitOfWork;
    }
}

WriteUnitOfWork::~WriteUnitOfWork() {
    dassert(!storageGlobalParams.readOnly);
    if (!_released && !_committed) {
        invariant(_opCtx->_ruState != OperationContext::kNotInUnitOfWork);
        if (_toplevel) {
            _opCtx->recoveryUnit()->abortUnitOfWork();
            _opCtx->_ruState = OperationContext::kNotInUnitOfWork;
        } else {
            _opCtx->_ruState = OperationContext::kFailedUnitOfWork;
        }
        _opCtx->lockState()->endWriteUnitOfWork();
    }
}

/**
 * Creates a top-level WriteUnitOfWork without changing RecoveryUnit or Locker state. For use
 * when the RecoveryUnit and Locker are already in an active state.
 */
std::unique_ptr<WriteUnitOfWork> WriteUnitOfWork::createForSnapshotResume(OperationContext* opCtx) {
    auto wuow = stdx::make_unique<WriteUnitOfWork>();
    wuow->_opCtx = opCtx;
    wuow->_toplevel = true;
    wuow->_opCtx->_ruState = OperationContext::kActiveUnitOfWork;
    return wuow;
}

/**
 * Releases the OperationContext RecoveryUnit and Locker objects from management without
 * changing state. Allows for use of these objects beyond the WriteUnitOfWork lifespan.
 */
void WriteUnitOfWork::release() {
    invariant(_opCtx->_ruState == OperationContext::kActiveUnitOfWork);
    invariant(!_committed);
    invariant(_toplevel);

    _released = true;
    _opCtx->_ruState = OperationContext::kNotInUnitOfWork;
}

void WriteUnitOfWork::commit() {
    invariant(!_committed);
    invariant(!_released);
    invariant(_opCtx->_ruState == OperationContext::kActiveUnitOfWork);
    if (_toplevel) {
        _opCtx->recoveryUnit()->commitUnitOfWork();
        _opCtx->_ruState = OperationContext::kNotInUnitOfWork;
    }
    _opCtx->lockState()->endWriteUnitOfWork();
    _committed = true;
}

}  // namespace mongo
