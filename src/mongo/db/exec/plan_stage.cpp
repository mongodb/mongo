/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kQuery

#include "mongo/platform/basic.h"

#include "mongo/db/exec/plan_stage.h"

#include "mongo/db/exec/scoped_timer.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"

namespace mongo {

PlanStage::StageState PlanStage::work(WorkingSetID* out) {
    invariant(_opCtx);
    ScopedTimer timer(getClock(), &_commonStats.executionTimeMillis);
    ++_commonStats.works;

    StageState workResult = doWork(out);

    if (StageState::ADVANCED == workResult) {
        ++_commonStats.advanced;
    } else if (StageState::NEED_TIME == workResult) {
        ++_commonStats.needTime;
    } else if (StageState::NEED_YIELD == workResult) {
        ++_commonStats.needYield;
    }

    return workResult;
}

void PlanStage::saveState() {
    ++_commonStats.yields;
    for (auto&& child : _children) {
        child->saveState();
    }

    doSaveState();
}

void PlanStage::restoreState() {
    ++_commonStats.unyields;
    for (auto&& child : _children) {
        child->restoreState();
    }

    doRestoreState();
}

void PlanStage::invalidate(OperationContext* txn, const RecordId& dl, InvalidationType type) {
    ++_commonStats.invalidates;
    for (auto&& child : _children) {
        child->invalidate(txn, dl, type);
    }

    doInvalidate(txn, dl, type);
}

void PlanStage::detachFromOperationContext() {
    invariant(_opCtx);
    _opCtx = nullptr;

    for (auto&& child : _children) {
        child->detachFromOperationContext();
    }

    doDetachFromOperationContext();
}

void PlanStage::reattachToOperationContext(OperationContext* opCtx) {
    invariant(_opCtx == nullptr);
    _opCtx = opCtx;

    for (auto&& child : _children) {
        child->reattachToOperationContext(opCtx);
    }

    doReattachToOperationContext();
}

ClockSource* PlanStage::getClock() const {
    return _opCtx->getServiceContext()->getFastClockSource();
}

}  // namespace mongo
