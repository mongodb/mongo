// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/classic/multi_iterator.h"

#include "mongo/db/query/plan_executor_impl.h"
#include "mongo/db/record_id.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/storage/record_data.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/util/assert_util.h"

#include <memory>
#include <utility>

#include <boost/optional/optional.hpp>

namespace mongo {

using std::unique_ptr;


MultiIteratorStage::MultiIteratorStage(ExpressionContext* expCtx,
                                       WorkingSet* ws,
                                       CollectionAcquisition collection)
    : RequiresCollectionStage(kStageType, expCtx, collection), _ws(ws) {}

void MultiIteratorStage::addIterator(unique_ptr<RecordCursor> it) {
    _iterators.push_back(std::move(it));
}

PlanStage::StageState MultiIteratorStage::doWork(WorkingSetID* out) {
    boost::optional<Record> record;

    const auto ret = handlePlanStageYield(
        expCtx(),
        "MultiIteratorStage",
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
            tassert(11051638,
                    "Expecting iterators not to advance in case of write conflict",
                    !_iterators.empty());
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
    member->resetDocument(shard_role_details::getRecoveryUnit(opCtx())->getSnapshotId(),
                          record->data.releaseToBson());
    _ws->transitionToRecordIdAndObj(*out);
    return PlanStage::ADVANCED;
}

bool MultiIteratorStage::isEOF() const {
    return _iterators.empty();
}

void MultiIteratorStage::doSaveStateRequiresCollection() {
    for (auto&& iterator : _iterators) {
        iterator->save();
    }
}

void MultiIteratorStage::doRestoreStateRequiresCollection() {
    for (auto&& iterator : _iterators) {
        const bool couldRestore = iterator->restore(*shard_role_details::getRecoveryUnit(opCtx()));
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
