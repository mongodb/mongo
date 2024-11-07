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
#include <vector>

#include <boost/container/small_vector.hpp>
// IWYU pragma: no_include "boost/intrusive/detail/iterator.hpp"
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/db/catalog/collection.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/query/plan_executor_impl.h"
#include "mongo/db/record_id.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/util/assert_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {

using std::unique_ptr;

// static
const char* DistinctScan::kStageType = "DISTINCT_SCAN";

DistinctScan::DistinctScan(ExpressionContext* expCtx,
                           VariantCollectionPtrOrAcquisition collection,
                           DistinctParams params,
                           WorkingSet* workingSet,
                           std::unique_ptr<ShardFiltererImpl> shardFilterer,
                           bool needsFetch)
    : RequiresIndexStage(kStageType, expCtx, collection, params.indexDescriptor, workingSet),
      _workingSet(workingSet),
      _keyPattern(std::move(params.keyPattern)),
      _scanDirection(params.scanDirection),
      _bounds(std::move(params.bounds)),
      _fieldNo(params.fieldNo),
      _checker(&_bounds, _keyPattern, _scanDirection),
      _shardFilterer(std::move(shardFilterer)),
      _needsFetch(needsFetch) {
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
    _specificStats.isShardFiltering = _shardFilterer != nullptr;
    _specificStats.isFetching = _needsFetch;
    _specificStats.isShardFilteringDistinctScanEnabled =
        expCtx->isFeatureFlagShardFilteringDistinctScanEnabled();

    // Set up our initial seek. If there is no valid data, just mark as EOF.
    _commonStats.isEOF = !_checker.getStartSeekPoint(&_seekPoint);
}

PlanStage::StageState DistinctScan::doFetch(WorkingSetMember* member,
                                            WorkingSetID id,
                                            WorkingSetID* out) {
    if (_idRetrying != WorkingSet::INVALID_ID) {
        id = _idRetrying;
        _idRetrying = WorkingSet::INVALID_ID;
    }

    return handlePlanStageYield(
        expCtx(),
        "DistinctScan",
        [&] {
            if (!_fetchCursor) {
                _fetchCursor = collectionPtr()->getCursor(opCtx());
            }

            if (!WorkingSetCommon::fetch(opCtx(),
                                         _workingSet,
                                         id,
                                         _fetchCursor.get(),
                                         collectionPtr(),
                                         collectionPtr()->ns())) {
                _workingSet->free(id);
                return NEED_TIME;
            }

            return ADVANCED;
        },
        [&] {
            // Ensure that the BSONObj underlying the WorkingSetMember is owned because it may be
            // freed when we yield.
            member->makeObjOwnedIfNeeded();
            _idRetrying = id;
            *out = WorkingSet::INVALID_ID;
        } /* yieldHandler */);
}

PlanStage::StageState DistinctScan::doWork(WorkingSetID* out) {
    if (_commonStats.isEOF)
        return PlanStage::IS_EOF;

    boost::optional<IndexKeyEntry> kv;
    const auto ret = handlePlanStageYield(
        expCtx(),
        "DistinctScan",
        [&] {
            if (!_cursor) {
                _cursor = indexAccessMethod()->newCursor(opCtx(), _scanDirection == 1);
            } else if (_needsFetch && _idRetrying != WorkingSet::INVALID_ID) {
                // We're retrying a fetch! Don't call seek() or next().
                return PlanStage::ADVANCED;
            }

            if (_needsSequentialScan) {
                kv = _cursor->next();
                _needsSequentialScan = false;
                return PlanStage::ADVANCED;
            }

            key_string::Builder builder(
                indexAccessMethod()->getSortedDataInterface()->getKeyStringVersion(),
                indexAccessMethod()->getSortedDataInterface()->getOrdering());
            kv = _cursor->seek(IndexEntryComparison::makeKeyStringFromSeekPointForSeek(
                _seekPoint, _scanDirection == 1, builder));
            return PlanStage::ADVANCED;
        },
        [&] {
            *out = WorkingSet::INVALID_ID;
        } /* yieldHandler */);

    if (ret != PlanStage::ADVANCED) {
        return ret;
    }

    if (!kv) {
        _commonStats.isEOF = true;
        return PlanStage::IS_EOF;
    }

    ++_specificStats.keysExamined;

    switch (_checker.checkKey(kv->key, &_seekPoint)) {
        case IndexBoundsChecker::MUST_ADVANCE: {
            // Try again next time. The checker has adjusted the _seekPoint.
            return PlanStage::NEED_TIME;
        }
        case IndexBoundsChecker::DONE: {
            // There won't be a next time.
            _commonStats.isEOF = true;
            _cursor.reset();
            return IS_EOF;
        }
        case IndexBoundsChecker::VALID: {
            if (!kv->key.isOwned())
                kv->key = kv->key.getOwned();

            // Package up the result for the caller.
            WorkingSetID id = _workingSet->allocate();
            WorkingSetMember* member = _workingSet->get(id);
            member->recordId = kv->loc;
            member->keyData.push_back(
                IndexKeyDatum(_keyPattern,
                              kv->key,
                              workingSetIndexId(),
                              shard_role_details::getRecoveryUnit(opCtx())->getSnapshotId()));
            _workingSet->transitionToRecordIdAndIdx(id);

            if (_needsFetch) {
                const auto fetchRet = doFetch(member, id, out);
                if (fetchRet != PlanStage::ADVANCED) {
                    return fetchRet;
                }
            }

            // We need one last check before we can return the key if we've been initialized with a
            // shard filter. If this document is an orphan, we need to try the next one; otherwise,
            // we can proceed.
            const auto belongs = _shardFilterer ? _shardFilterer->documentBelongsToMe(*member)
                                                : ShardFilterer::DocumentBelongsResult::kBelongs;

            switch (belongs) {
                case ShardFilterer::DocumentBelongsResult::kBelongs: {
                    // Adjust the _seekPoint so that it is exclusive on the field we are using.
                    _seekPoint.keyPrefix = kv->key;
                    _seekPoint.prefixLen = _fieldNo + 1;
                    _seekPoint.firstExclusive = _fieldNo;

                    // Return the current entry.
                    *out = id;
                    return PlanStage::ADVANCED;
                }
                case ShardFilterer::DocumentBelongsResult::kNoShardKey: {
                    tassert(9245300, "Covering index failed to provide shard key", _needsFetch);

                    // Skip this working set member with a warning - no shard key should not be
                    // possible unless manually inserting data into a shard.
                    tassert(9245400, "Expected document to be fetched", member->hasObj());
                    LOGV2_WARNING(
                        9245401,
                        "No shard key found in document, it may have been inserted manually "
                        "into shard",
                        "document"_attr = redact(member->doc.value().toBson()),
                        "keyPattern"_attr = _shardFilterer->getKeyPattern());
                    [[fallthrough]];
                }
                case ShardFilterer::DocumentBelongsResult::kDoesNotBelong: {
                    // We found an orphan; we need to try the next entry in the index in case its
                    // not an orphan.
                    _needsSequentialScan = true;
                    _workingSet->free(id);
                    return PlanStage::NEED_TIME;
                }
                default:
                    MONGO_UNREACHABLE_TASSERT(9245301);
            }
        }
    }
    MONGO_UNREACHABLE_TASSERT(9245303);
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
