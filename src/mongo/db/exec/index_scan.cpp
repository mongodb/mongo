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

#include "mongo/db/exec/index_scan.h"

#include <memory>

#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/exec/filter.h"
#include "mongo/db/exec/scoped_timer.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index_names.h"
#include "mongo/db/query/index_bounds_builder.h"
#include "mongo/db/query/plan_executor_impl.h"
#include "mongo/util/assert_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


namespace {

// Return a value in the set {-1, 0, 1} to represent the sign of parameter i.
int sgn(int i) {
    if (i == 0)
        return 0;
    return i > 0 ? 1 : -1;
}

}  // namespace

namespace mongo {

// static
const char* IndexScan::kStageType = "IXSCAN";

IndexScan::IndexScan(ExpressionContext* expCtx,
                     const CollectionPtr& collection,
                     IndexScanParams params,
                     WorkingSet* workingSet,
                     const MatchExpression* filter)
    : RequiresIndexStage(kStageType, expCtx, collection, params.indexDescriptor, workingSet),
      _workingSet(workingSet),
      _keyPattern(params.keyPattern.getOwned()),
      _bounds(std::move(params.bounds)),
      _filter((filter && !filter->isTriviallyTrue()) ? filter : nullptr),
      _direction(params.direction),
      _forward(params.direction == 1),
      _shouldDedup(params.shouldDedup),
      _addKeyMetadata(params.addKeyMetadata),
      _startKeyInclusive(IndexBounds::isStartIncludedInBound(params.bounds.boundInclusion)),
      _endKeyInclusive(IndexBounds::isEndIncludedInBound(params.bounds.boundInclusion)) {
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
}

boost::optional<IndexKeyEntry> IndexScan::initIndexScan() {
    // Perform the possibly heavy-duty initialization of the underlying index cursor.
    _indexCursor = indexAccessMethod()->newCursor(opCtx(), _forward);

    // We always seek once to establish the cursor position.
    ++_specificStats.seeks;

    if (_bounds.isSimpleRange) {
        // Start at one key, end at another.
        _startKey = _bounds.startKey;
        _endKey = _bounds.endKey;
        _indexCursor->setEndPosition(_endKey, _endKeyInclusive);

        KeyString::Value keyStringForSeek = IndexEntryComparison::makeKeyStringFromBSONKeyForSeek(
            _startKey,
            indexAccessMethod()->getSortedDataInterface()->getKeyStringVersion(),
            indexAccessMethod()->getSortedDataInterface()->getOrdering(),
            _forward,
            _startKeyInclusive);
        return _indexCursor->seek(keyStringForSeek);
    } else {
        // For single intervals, we can use an optimized scan which checks against the position
        // of an end cursor.  For all other index scans, we fall back on using
        // IndexBoundsChecker to determine when we've finished the scan.
        if (IndexBoundsBuilder::isSingleInterval(
                _bounds, &_startKey, &_startKeyInclusive, &_endKey, &_endKeyInclusive)) {
            _indexCursor->setEndPosition(_endKey, _endKeyInclusive);

            auto keyStringForSeek = IndexEntryComparison::makeKeyStringFromBSONKeyForSeek(
                _startKey,
                indexAccessMethod()->getSortedDataInterface()->getKeyStringVersion(),
                indexAccessMethod()->getSortedDataInterface()->getOrdering(),
                _forward,
                _startKeyInclusive);
            return _indexCursor->seek(keyStringForSeek);
        } else {
            _checker.reset(new IndexBoundsChecker(&_bounds, _keyPattern, _direction));

            if (!_checker->getStartSeekPoint(&_seekPoint))
                return boost::none;
            return _indexCursor->seek(IndexEntryComparison::makeKeyStringFromSeekPointForSeek(
                _seekPoint,
                indexAccessMethod()->getSortedDataInterface()->getKeyStringVersion(),
                indexAccessMethod()->getSortedDataInterface()->getOrdering(),
                _forward));
        }
    }
}

PlanStage::StageState IndexScan::doWork(WorkingSetID* out) {
    // Get the next kv pair from the index, if any.
    boost::optional<IndexKeyEntry> kv;

    const auto ret = handlePlanStageYield(
        expCtx(),
        "IndexScan",
        collection()->ns().ns(),
        [&] {
            switch (_scanState) {
                case INITIALIZING:
                    kv = initIndexScan();
                    break;
                case GETTING_NEXT:
                    kv = _indexCursor->next();
                    break;
                case NEED_SEEK:
                    ++_specificStats.seeks;
                    kv = _indexCursor->seek(IndexEntryComparison::makeKeyStringFromSeekPointForSeek(
                        _seekPoint,
                        indexAccessMethod()->getSortedDataInterface()->getKeyStringVersion(),
                        indexAccessMethod()->getSortedDataInterface()->getOrdering(),
                        _forward));
                    break;
                case HIT_END:
                    return PlanStage::IS_EOF;
            }
            return PlanStage::ADVANCED;
        },
        [&] {
            // yieldHandler
            *out = WorkingSet::INVALID_ID;
        });

    if (ret != PlanStage::ADVANCED) {
        return ret;
    }

    if (kv) {
        // In debug mode, check that the cursor isn't lying to us.
        if (kDebugBuild && !_startKey.isEmpty()) {
            int cmp = kv->key.woCompare(_startKey,
                                        Ordering::make(_keyPattern),
                                        /*compareFieldNames*/ false);
            if (cmp == 0)
                dassert(_startKeyInclusive);
            dassert(_forward ? cmp >= 0 : cmp <= 0);
        }

        if (kDebugBuild && !_endKey.isEmpty()) {
            int cmp = kv->key.woCompare(_endKey,
                                        Ordering::make(_keyPattern),
                                        /*compareFieldNames*/ false);
            if (cmp == 0)
                dassert(_endKeyInclusive);
            dassert(_forward ? cmp <= 0 : cmp >= 0);
        }

        ++_specificStats.keysExamined;
    }

    if (kv && _checker) {
        switch (_checker->checkKey(kv->key, &_seekPoint)) {
            case IndexBoundsChecker::VALID:
                break;

            case IndexBoundsChecker::DONE:
                kv = boost::none;
                break;

            case IndexBoundsChecker::MUST_ADVANCE:
                _scanState = NEED_SEEK;
                return PlanStage::NEED_TIME;
        }
    }

    if (!kv) {
        _scanState = HIT_END;
        _commonStats.isEOF = true;
        _indexCursor.reset();
        return PlanStage::IS_EOF;
    }

    _scanState = GETTING_NEXT;

    if (_shouldDedup) {
        ++_specificStats.dupsTested;
        if (!_returned.insert(kv->loc).second) {
            // We've seen this RecordId before. Skip it this time.
            ++_specificStats.dupsDropped;
            return PlanStage::NEED_TIME;
        }
    }

    if (!Filter::passes(kv->key, _keyPattern, _filter)) {
        return PlanStage::NEED_TIME;
    }

    if (!kv->key.isOwned())
        kv->key = kv->key.getOwned();

    // We found something to return, so fill out the WSM.
    WorkingSetID id = _workingSet->allocate();
    WorkingSetMember* member = _workingSet->get(id);
    member->recordId = std::move(kv->loc);
    member->keyData.push_back(IndexKeyDatum(
        _keyPattern, kv->key, workingSetIndexId(), opCtx()->recoveryUnit()->getSnapshotId()));
    _workingSet->transitionToRecordIdAndIdx(id);

    if (_addKeyMetadata) {
        member->metadata().setIndexKey(IndexKeyEntry::rehydrateKey(_keyPattern, kv->key));
    }

    *out = id;
    return PlanStage::ADVANCED;
}

bool IndexScan::isEOF() {
    return _commonStats.isEOF;
}

void IndexScan::doSaveStateRequiresIndex() {
    if (!_indexCursor)
        return;

    if (_scanState == NEED_SEEK) {
        _indexCursor->saveUnpositioned();
        return;
    }

    _indexCursor->save();
}

void IndexScan::doRestoreStateRequiresIndex() {
    if (_indexCursor)
        _indexCursor->restore();
}

void IndexScan::doDetachFromOperationContext() {
    if (_indexCursor)
        _indexCursor->detachFromOperationContext();
}

void IndexScan::doReattachToOperationContext() {
    if (_indexCursor)
        _indexCursor->reattachToOperationContext(opCtx());
}

std::unique_ptr<PlanStageStats> IndexScan::getStats() {
    // WARNING: this could be called even if the collection was dropped.  Do not access any
    // catalog information here.

    // Add a BSON representation of the filter to the stats tree, if there is one.
    if (nullptr != _filter) {
        BSONObjBuilder bob;
        _filter->serialize(&bob);
        _commonStats.filter = bob.obj();
    }

    // These specific stats fields never change.
    if (_specificStats.indexType.empty()) {
        _specificStats.indexType = "BtreeCursor";  // TODO amName;

        _specificStats.indexBounds = _bounds.toBSON(!_specificStats.collation.isEmpty());

        _specificStats.direction = _direction;
    }

    std::unique_ptr<PlanStageStats> ret =
        std::make_unique<PlanStageStats>(_commonStats, STAGE_IXSCAN);
    ret->specific = std::make_unique<IndexScanStats>(_specificStats);
    return ret;
}

}  // namespace mongo
