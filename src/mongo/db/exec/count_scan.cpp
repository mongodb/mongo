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

#include "mongo/db/exec/count_scan.h"

#include <memory>

#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/exec/scoped_timer.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/query/plan_executor_impl.h"
#include "mongo/util/assert_util.h"

namespace mongo {

namespace {
/**
 * This function replaces field names in *replace* with those from the object
 * *fieldNames*, preserving field ordering.  Both objects must have the same
 * number of fields.
 *
 * Example:
 *
 *     replaceBSONKeyNames({ 'a': 1, 'b' : 1 }, { '': 'foo', '', 'bar' }) =>
 *
 *         { 'a' : 'foo' }, { 'b' : 'bar' }
 */
BSONObj replaceBSONFieldNames(const BSONObj& replace, const BSONObj& fieldNames) {
    invariant(replace.nFields() == fieldNames.nFields());

    BSONObjBuilder bob;
    auto iter = fieldNames.begin();

    for (const BSONElement& el : replace) {
        bob.appendAs(el, (*iter++).fieldNameStringData());
    }

    return bob.obj();
}
}  // namespace

using std::unique_ptr;
using std::vector;

// static
const char* CountScan::kStageType = "COUNT_SCAN";

// When building the CountScan stage we take the keyPattern, index name, and multikey details from
// the CountScanParams rather than resolving them via the IndexDescriptor, since these may differ
// from the descriptor's contents.
CountScan::CountScan(ExpressionContext* expCtx,
                     const CollectionPtr& collection,
                     CountScanParams params,
                     WorkingSet* workingSet)
    : RequiresIndexStage(kStageType, expCtx, collection, params.indexDescriptor, workingSet),
      _workingSet(workingSet),
      _keyPattern(std::move(params.keyPattern)),
      _shouldDedup(params.isMultiKey),
      _startKey(std::move(params.startKey)),
      _startKeyInclusive(params.startKeyInclusive),
      _endKey(std::move(params.endKey)),
      _endKeyInclusive(params.endKeyInclusive) {
    _specificStats.indexName = params.name;
    _specificStats.keyPattern = _keyPattern;
    _specificStats.isMultiKey = params.isMultiKey;
    _specificStats.multiKeyPaths = params.multikeyPaths;
    _specificStats.isUnique = params.indexDescriptor->unique();
    _specificStats.isSparse = params.indexDescriptor->isSparse();
    _specificStats.isPartial = params.indexDescriptor->isPartial();
    _specificStats.indexVersion = static_cast<int>(params.indexDescriptor->version());
    _specificStats.collation = params.indexDescriptor->infoObj()
                                   .getObjectField(IndexDescriptor::kCollationFieldName)
                                   .getOwned();

    // endKey must be after startKey in index order since we only do forward scans.
    dassert(_startKey.woCompare(_endKey,
                                Ordering::make(_keyPattern),
                                /*compareFieldNames*/ false) <= 0);
}

PlanStage::StageState CountScan::doWork(WorkingSetID* out) {
    if (_commonStats.isEOF)
        return PlanStage::IS_EOF;

    boost::optional<IndexKeyEntry> entry;
    const bool needInit = !_cursor;

    const auto ret = handlePlanStageYield(
        expCtx(),
        "CountScan",
        collection()->ns().ns(),
        [&] {
            // We don't care about the keys.
            const auto kWantLoc = SortedDataInterface::Cursor::kWantLoc;

            if (needInit) {
                // First call to work().  Perform cursor init.
                _cursor = indexAccessMethod()->newCursor(opCtx());
                _cursor->setEndPosition(_endKey, _endKeyInclusive);

                auto keyStringForSeek = IndexEntryComparison::makeKeyStringFromBSONKeyForSeek(
                    _startKey,
                    indexAccessMethod()->getSortedDataInterface()->getKeyStringVersion(),
                    indexAccessMethod()->getSortedDataInterface()->getOrdering(),
                    true, /* forward */
                    _startKeyInclusive);
                entry = _cursor->seek(keyStringForSeek);
            } else {
                entry = _cursor->next(kWantLoc);
            }
            return PlanStage::ADVANCED;
        },
        [&] {
            // yieldHandler
            if (needInit) {
                // Release our cursor and try again next time.
                _cursor.reset();
            }
            *out = WorkingSet::INVALID_ID;
        });

    if (ret != PlanStage::ADVANCED) {
        return ret;
    }

    ++_specificStats.keysExamined;

    if (!entry) {
        _commonStats.isEOF = true;
        _cursor.reset();
        return PlanStage::IS_EOF;
    }

    if (_shouldDedup && !_returned.insert(entry->loc).second) {
        // *loc was already in _returned.
        return PlanStage::NEED_TIME;
    }

    WorkingSetID id = _workingSet->allocate();
    WorkingSetMember* member = _workingSet->get(id);
    member->recordId = entry->loc;
    _workingSet->transitionToRecordIdAndObj(id);
    *out = id;
    return PlanStage::ADVANCED;
}

bool CountScan::isEOF() {
    return _commonStats.isEOF;
}

void CountScan::doSaveStateRequiresIndex() {
    if (_cursor)
        _cursor->save();
}

void CountScan::doRestoreStateRequiresIndex() {
    if (_cursor)
        _cursor->restore();
}

void CountScan::doDetachFromOperationContext() {
    if (_cursor)
        _cursor->detachFromOperationContext();
}

void CountScan::doReattachToOperationContext() {
    if (_cursor)
        _cursor->reattachToOperationContext(opCtx());
}

unique_ptr<PlanStageStats> CountScan::getStats() {
    unique_ptr<PlanStageStats> ret =
        std::make_unique<PlanStageStats>(_commonStats, STAGE_COUNT_SCAN);

    unique_ptr<CountScanStats> countStats = std::make_unique<CountScanStats>(_specificStats);
    countStats->keyPattern = _specificStats.keyPattern.getOwned();

    countStats->startKey = replaceBSONFieldNames(_startKey, countStats->keyPattern);
    countStats->startKeyInclusive = _startKeyInclusive;
    countStats->endKey = replaceBSONFieldNames(_endKey, countStats->keyPattern);
    countStats->endKeyInclusive = _endKeyInclusive;

    ret->specific = std::move(countStats);

    return ret;
}

const SpecificStats* CountScan::getSpecificStats() const {
    return &_specificStats;
}

}  // namespace mongo
