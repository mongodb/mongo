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

#include "mongo/db/exec/multi_iterator.h"

#include <memory>

#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/query/plan_executor_impl.h"
#include "mongo/util/assert_util.h"

namespace mongo {

using std::unique_ptr;
using std::vector;

const char* MultiIteratorStage::kStageType = "MULTI_ITERATOR";

MultiIteratorStage::MultiIteratorStage(ExpressionContext* expCtx,
                                       WorkingSet* ws,
                                       const CollectionPtr& collection)
    : RequiresCollectionStage(kStageType, expCtx, collection), _ws(ws) {}

void MultiIteratorStage::addIterator(unique_ptr<RecordCursor> it) {
    _iterators.push_back(std::move(it));
}

PlanStage::StageState MultiIteratorStage::doWork(WorkingSetID* out) {
    boost::optional<Record> record;

    const auto ret = handlePlanStageYield(expCtx(),
                                          "MultiIteratorStage",
                                          collection()->ns().ns(),
                                          [&] {
                                              while (!_iterators.empty()) {
                                                  record = _iterators.back()->next();
                                                  if (record)
                                                      break;
                                                  _iterators.pop_back();
                                              }
                                              return PlanStage::ADVANCED;
                                          },
                                          [&] {
                                              // yieldHandler
                                              // If _advance throws a WCE we shouldn't have moved.
                                              invariant(!_iterators.empty());
                                              *out = WorkingSet::INVALID_ID;
                                          });

    if (ret != PlanStage::ADVANCED) {
        return ret;
    }

    if (!record)
        return IS_EOF;

    *out = _ws->allocate();
    WorkingSetMember* member = _ws->get(*out);
    member->recordId = std::move(record->id);
    member->resetDocument(opCtx()->recoveryUnit()->getSnapshotId(), record->data.releaseToBson());
    _ws->transitionToRecordIdAndObj(*out);
    return PlanStage::ADVANCED;
}

bool MultiIteratorStage::isEOF() {
    return _iterators.empty();
}

void MultiIteratorStage::doSaveStateRequiresCollection() {
    for (auto&& iterator : _iterators) {
        iterator->save();
    }
}

void MultiIteratorStage::doRestoreStateRequiresCollection() {
    for (auto&& iterator : _iterators) {
        const bool couldRestore = iterator->restore();
        uassert(50991, "could not restore cursor for MULTI_ITERATOR stage", couldRestore);
    }
}

void MultiIteratorStage::doDetachFromOperationContext() {
    for (auto&& iterator : _iterators) {
        iterator->detachFromOperationContext();
    }
}

void MultiIteratorStage::doReattachToOperationContext() {
    for (auto&& iterator : _iterators) {
        iterator->reattachToOperationContext(opCtx());
    }
}

unique_ptr<PlanStageStats> MultiIteratorStage::getStats() {
    unique_ptr<PlanStageStats> ret =
        std::make_unique<PlanStageStats>(_commonStats, STAGE_MULTI_ITERATOR);
    ret->specific = std::make_unique<CollectionScanStats>();
    return ret;
}

}  // namespace mongo
