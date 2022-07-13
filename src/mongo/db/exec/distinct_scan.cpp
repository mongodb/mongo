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

#include "mongo/db/exec/distinct_scan.h"

#include <memory>

#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/exec/filter.h"
#include "mongo/db/exec/scoped_timer.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/query/plan_executor_impl.h"
#include "mongo/util/assert_util.h"

namespace mongo {

using std::unique_ptr;
using std::vector;

// static
const char* DistinctScan::kStageType = "DISTINCT_SCAN";

DistinctScan::DistinctScan(ExpressionContext* expCtx,
                           const CollectionPtr& collection,
                           DistinctParams params,
                           WorkingSet* workingSet)
    : RequiresIndexStage(kStageType, expCtx, collection, params.indexDescriptor, workingSet),
      _workingSet(workingSet),
      _keyPattern(std::move(params.keyPattern)),
      _scanDirection(params.scanDirection),
      _bounds(std::move(params.bounds)),
      _fieldNo(params.fieldNo),
      _checker(&_bounds, _keyPattern, _scanDirection) {
    _specificStats.keyPattern = _keyPattern;
    _specificStats.indexName = params.name;
    _specificStats.indexVersion = static_cast<int>(params.indexDescriptor->version());
    _specificStats.isMultiKey = params.isMultiKey;
    _specificStats.multiKeyPaths = params.multikeyPaths;
    _specificStats.isUnique = params.indexDescriptor->unique();
    _specificStats.isSparse = params.indexDescriptor->isSparse();
    _specificStats.isPartial = params.indexDescriptor->isPartial();
    _specificStats.direction = _scanDirection;
    _specificStats.collation = params.indexDescriptor->infoObj()
                                   .getObjectField(IndexDescriptor::kCollationFieldName)
                                   .getOwned();

    // Set up our initial seek. If there is no valid data, just mark as EOF.
    _commonStats.isEOF = !_checker.getStartSeekPoint(&_seekPoint);
}

PlanStage::StageState DistinctScan::doWork(WorkingSetID* out) {
    if (_commonStats.isEOF)
        return PlanStage::IS_EOF;

    boost::optional<IndexKeyEntry> kv;

    const auto ret = handlePlanStageYield(
        expCtx(),
        "DistinctScan",
        collection()->ns().ns(),
        [&] {
            if (!_cursor)
                _cursor = indexAccessMethod()->newCursor(opCtx(), _scanDirection == 1);
            kv = _cursor->seek(IndexEntryComparison::makeKeyStringFromSeekPointForSeek(
                _seekPoint,
                indexAccessMethod()->getSortedDataInterface()->getKeyStringVersion(),
                indexAccessMethod()->getSortedDataInterface()->getOrdering(),
                _scanDirection == 1));
            return PlanStage::ADVANCED;
        },
        [&] {
            // yieldHandler
            *out = WorkingSet::INVALID_ID;
        });

    if (ret != PlanStage::ADVANCED) {
        return ret;
    }

    if (!kv) {
        _commonStats.isEOF = true;
        return PlanStage::IS_EOF;
    }

    ++_specificStats.keysExamined;

    switch (_checker.checkKey(kv->key, &_seekPoint)) {
        case IndexBoundsChecker::MUST_ADVANCE:
            // Try again next time. The checker has adjusted the _seekPoint.
            return PlanStage::NEED_TIME;

        case IndexBoundsChecker::DONE:
            // There won't be a next time.
            _commonStats.isEOF = true;
            _cursor.reset();
            return IS_EOF;

        case IndexBoundsChecker::VALID:
            // Return this key. Adjust the _seekPoint so that it is exclusive on the field we
            // are using.

            if (!kv->key.isOwned())
                kv->key = kv->key.getOwned();
            _seekPoint.keyPrefix = kv->key;
            _seekPoint.prefixLen = _fieldNo + 1;
            _seekPoint.firstExclusive = _fieldNo;

            // Package up the result for the caller.
            WorkingSetID id = _workingSet->allocate();
            WorkingSetMember* member = _workingSet->get(id);
            member->recordId = kv->loc;
            member->keyData.push_back(IndexKeyDatum(_keyPattern,
                                                    kv->key,
                                                    workingSetIndexId(),
                                                    opCtx()->recoveryUnit()->getSnapshotId()));
            _workingSet->transitionToRecordIdAndIdx(id);

            *out = id;
            return PlanStage::ADVANCED;
    }
    MONGO_UNREACHABLE;
}

bool DistinctScan::isEOF() {
    return _commonStats.isEOF;
}

void DistinctScan::doSaveStateRequiresIndex() {
    // We always seek, so we don't care where the cursor is.
    if (_cursor)
        _cursor->saveUnpositioned();
}

void DistinctScan::doRestoreStateRequiresIndex() {
    if (_cursor)
        _cursor->restore();
}

void DistinctScan::doDetachFromOperationContext() {
    if (_cursor)
        _cursor->detachFromOperationContext();
}

void DistinctScan::doReattachToOperationContext() {
    if (_cursor)
        _cursor->reattachToOperationContext(opCtx());
}

unique_ptr<PlanStageStats> DistinctScan::getStats() {
    // Serialize the bounds to BSON if we have not done so already. This is done here rather than in
    // the constructor in order to avoid the expensive serialization operation unless the distinct
    // command is being explained.
    if (_specificStats.indexBounds.isEmpty()) {
        _specificStats.indexBounds = _bounds.toBSON(!_specificStats.collation.isEmpty());
    }

    unique_ptr<PlanStageStats> ret =
        std::make_unique<PlanStageStats>(_commonStats, STAGE_DISTINCT_SCAN);
    ret->specific = std::make_unique<DistinctScanStats>(_specificStats);
    return ret;
}

const SpecificStats* DistinctScan::getSpecificStats() const {
    return &_specificStats;
}

}  // namespace mongo
