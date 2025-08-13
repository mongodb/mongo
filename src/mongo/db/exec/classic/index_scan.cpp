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


// IWYU pragma: no_include "boost/intrusive/detail/iterator.hpp"
#include "mongo/db/exec/classic/index_scan.h"

#include "mongo/db/exec/classic/filter.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/query/compiler/optimizer/index_bounds_builder/index_bounds_builder.h"
#include "mongo/db/query/plan_executor_impl.h"
#include "mongo/db/storage/exceptions.h"
#include "mongo/db/storage/key_string/key_string.h"
#include "mongo/util/assert_util.h"

#include <boost/none.hpp>

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

MONGO_FAIL_POINT_DEFINE(throwDuringIndexScanRestore);

// static
const char* IndexScan::kStageType = "IXSCAN";

IndexScan::IndexScan(ExpressionContext* expCtx,
                     VariantCollectionPtrOrAcquisition collection,
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
      _addKeyMetadata(params.addKeyMetadata),
      _dedup(params.shouldDedup),
      _recordIdDeduplicator(expCtx),
      _startKeyInclusive(IndexBounds::isStartIncludedInBound(_bounds.boundInclusion)),
      _endKeyInclusive(IndexBounds::isEndIncludedInBound(_bounds.boundInclusion)),
      // TODO SERVER-97747 Add internalIndexScanMaxMemoryBytes when it exists.
      _memoryTracker(OperationMemoryUsageTracker::createSimpleMemoryUsageTrackerForStage(*expCtx)) {
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
    auto& ru = *shard_role_details::getRecoveryUnit(opCtx());
    // Perform the possibly heavy-duty initialization of the underlying index cursor.
    _indexCursor = indexAccessMethod()->newCursor(opCtx(), ru, _forward);

    // We always seek once to establish the cursor position.
    ++_specificStats.seeks;

    if (_bounds.isSimpleRange) {
        // Start at one key, end at another.
        _startKey = _bounds.startKey;
        _endKey = _bounds.endKey;
        _indexCursor->setEndPosition(_endKey, _endKeyInclusive);

        key_string::Builder builder(
            indexAccessMethod()->getSortedDataInterface()->getKeyStringVersion());
        auto keyStringForSeek = IndexEntryComparison::makeKeyStringFromBSONKeyForSeek(
            _startKey,
            indexAccessMethod()->getSortedDataInterface()->getOrdering(),
            _forward,
            _startKeyInclusive,
            builder);
        return _indexCursor->seek(ru, keyStringForSeek);
    } else {
        // For single intervals, we can use an optimized scan which checks against the position
        // of an end cursor.  For all other index scans, we fall back on using
        // IndexBoundsChecker to determine when we've finished the scan.
        if (IndexBoundsBuilder::isSingleInterval(
                _bounds, &_startKey, &_startKeyInclusive, &_endKey, &_endKeyInclusive)) {
            _indexCursor->setEndPosition(_endKey, _endKeyInclusive);

            key_string::Builder builder(
                indexAccessMethod()->getSortedDataInterface()->getKeyStringVersion());
            auto keyStringForSeek = IndexEntryComparison::makeKeyStringFromBSONKeyForSeek(
                _startKey,
                indexAccessMethod()->getSortedDataInterface()->getOrdering(),
                _forward,
                _startKeyInclusive,
                builder);
            return _indexCursor->seek(ru, keyStringForSeek);
        } else {
            _checker.reset(new IndexBoundsChecker(&_bounds, _keyPattern, _direction));

            if (!_checker->getStartSeekPoint(&_seekPoint))
                return boost::none;
            key_string::Builder builder(
                indexAccessMethod()->getSortedDataInterface()->getKeyStringVersion(),
                indexAccessMethod()->getSortedDataInterface()->getOrdering());
            return _indexCursor->seek(ru,
                                      IndexEntryComparison::makeKeyStringFromSeekPointForSeek(
                                          _seekPoint, _forward, builder));
        }
    }
}

PlanStage::StageState IndexScan::doWork(WorkingSetID* out) {
    // Get the next kv pair from the index, if any.
    boost::optional<IndexKeyEntry> kv;

    const auto ret = handlePlanStageYield(
        expCtx(),
        "IndexScan",
        [&] {
            auto& ru = *shard_role_details::getRecoveryUnit(opCtx());
            switch (_scanState) {
                case INITIALIZING:
                    kv = initIndexScan();
                    break;
                case GETTING_NEXT:
                    kv = _indexCursor->next(ru);
                    break;
                case NEED_SEEK: {
                    ++_specificStats.seeks;
                    key_string::Builder builder(
                        indexAccessMethod()->getSortedDataInterface()->getKeyStringVersion(),
                        indexAccessMethod()->getSortedDataInterface()->getOrdering());
                    kv = _indexCursor->seek(ru,
                                            IndexEntryComparison::makeKeyStringFromSeekPointForSeek(
                                                _seekPoint, _forward, builder));
                    break;
                }
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
        _specificStats.peakTrackedMemBytes = _memoryTracker.peakTrackedMemoryBytes();
        return PlanStage::IS_EOF;
    }

    _scanState = GETTING_NEXT;

    // If we're deduping
    if (_dedup) {
        ++_specificStats.dupsTested;

        // ... check whether we have seen the record id.
        // Do not add the recordId to recordIdDeduplicator unless we know that the
        // scan will return the recordId.
        uint64_t dedupBytesPrev = _recordIdDeduplicator.getApproximateSize();
        bool duplicate = _filter == nullptr ? !_recordIdDeduplicator.insert(kv->loc)
                                            : _recordIdDeduplicator.contains(kv->loc);
        uint64_t dedupBytes = _recordIdDeduplicator.getApproximateSize();
        _memoryTracker.add(dedupBytes - dedupBytesPrev);
        _specificStats.peakTrackedMemBytes = _memoryTracker.peakTrackedMemoryBytes();

        // If we've seen the RecordId before
        if (duplicate) {
            // ...skip it
            ++_specificStats.dupsDropped;
            return PlanStage::NEED_TIME;
        }
    }

    if (!Filter::passes(kv->key, _keyPattern, _filter)) {
        return PlanStage::NEED_TIME;
    }

    // If we're deduping and the record matches a non-null filter
    if (_dedup && _filter != nullptr) {
        // ... now we can add the RecordId to the Deduplicator.
        uint64_t dedupBytesPrev = _recordIdDeduplicator.getApproximateSize();
        _recordIdDeduplicator.insert(kv->loc);
        uint64_t dedupBytes = _recordIdDeduplicator.getApproximateSize();
        _memoryTracker.add(dedupBytes - dedupBytesPrev);
        _specificStats.peakTrackedMemBytes = _memoryTracker.peakTrackedMemoryBytes();
    }

    if (!kv->key.isOwned())
        kv->key = kv->key.getOwned();

    // We found something to return, so fill out the WSM.
    WorkingSetID id = _workingSet->allocate();
    WorkingSetMember* member = _workingSet->get(id);
    member->recordId = std::move(kv->loc);
    member->keyData.push_back(
        IndexKeyDatum(_keyPattern,
                      kv->key,
                      workingSetIndexId(),
                      shard_role_details::getRecoveryUnit(opCtx())->getSnapshotId()));
    _workingSet->transitionToRecordIdAndIdx(id);

    if (_addKeyMetadata) {
        member->metadata().setIndexKey(IndexKeyEntry::rehydrateKey(_keyPattern, kv->key));
    }

    *out = id;
    return PlanStage::ADVANCED;
}

bool IndexScan::isEOF() const {
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
    if (_indexCursor) {
        if (MONGO_unlikely(throwDuringIndexScanRestore.shouldFail())) {
            throwTemporarilyUnavailableException(str::stream()
                                                 << "Hit failpoint '"
                                                 << throwDuringIndexScanRestore.getName() << "'.");
        }
        auto& ru = *shard_role_details::getRecoveryUnit(opCtx());
        _indexCursor->restore(ru);
    }
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
        _commonStats.filter = _filter->serialize();
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
