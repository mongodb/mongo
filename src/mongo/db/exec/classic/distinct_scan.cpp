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

#include "mongo/db/exec/classic/distinct_scan.h"

#include "mongo/db/exec/classic/orphan_chunk_skipper.h"

#include <memory>
#include <vector>

#include <boost/container/small_vector.hpp>
// IWYU pragma: no_include "boost/intrusive/detail/iterator.hpp"
#include "mongo/db/exec/classic/plan_stage.h"
#include "mongo/db/exec/classic/requires_index_stage.h"
#include "mongo/db/exec/classic/working_set.h"
#include "mongo/db/exec/classic/working_set_common.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/index_descriptor.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/query/plan_executor_impl.h"
#include "mongo/db/record_id.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/util/assert_util.h"

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

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

    // If we are shard-filtering and *not* fetching, that means our index includes the shard key. We
    // may be able to use this information to skip orphan chunks & hence do less work.
    if (_shardFilterer && !_needsFetch) {
        auto chunkSkipper = OrphanChunkSkipper::tryMakeChunkSkipper(
            *_shardFilterer,
            _shardFilterer->getFilter().getShardKeyPattern(),
            _keyPattern,
            _scanDirection);
        if (chunkSkipper) {
            _chunkSkipper.emplace(std::move(*chunkSkipper));
        }
    }

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
            auto& ru = *shard_role_details::getRecoveryUnit(opCtx());
            if (!_cursor) {
                _cursor = indexAccessMethod()->newCursor(opCtx(), ru, _scanDirection == 1);
            } else if (_needsFetch && _idRetrying != WorkingSet::INVALID_ID) {
                // We're retrying a fetch! Don't call seek() or next().
                return PlanStage::ADVANCED;
            }

            if (_needsSequentialScan) {
                kv = _cursor->next(ru);
                _needsSequentialScan = false;
                return PlanStage::ADVANCED;
            }

            key_string::Builder builder(
                indexAccessMethod()->getSortedDataInterface()->getKeyStringVersion(),
                indexAccessMethod()->getSortedDataInterface()->getOrdering());
            kv = _cursor->seek(ru,
                               IndexEntryComparison::makeKeyStringFromSeekPointForSeek(
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
                // We increment distinct scan's docsExamined as we would in the fetch stage.
                ++_specificStats.docsExamined;
                if (fetchRet != PlanStage::ADVANCED) {
                    return fetchRet;
                }
            }

            // Start by assuming this belongs to the current shard.
            auto belongs = ShardFilterer::DocumentBelongsResult::kBelongs;
            if (_chunkSkipper) {
                // If we have a chunk skipper, then we are potentially able to skip past orphan
                // chunks.
                auto info = _chunkSkipper->makeSeekPointIfOrphan(member->keyData, _seekPoint);
                switch (info) {
                    case OrphanChunkSkipper::NotOrphan: {
                        // This is not an orphan, so we can continue skipping distinct values as
                        // before. We handle that below.
                        break;
                    }
                    case mongo::OrphanChunkSkipper::CanSkipOrphans: {
                        // We have updated the seek point to skip past the current key (which is an
                        // orphan) to the next owned value of the shard key. Need to seek again.
                        _workingSet->free(id);
                        // This is a chunk-skip past one or more orphan chunks; increment stats
                        // counter.
                        ++_specificStats.orphanChunkSkips;
                        return PlanStage::NEED_TIME;
                    }
                    case mongo::OrphanChunkSkipper::NoMoreOwnedForThisPrefix: {
                        // Fall back to a distinct scan: we've exhausted our owned chunks for
                        // the prefix leading up to the shard key. Adjust the _seekPoint so that it
                        // is exclusive on the field we are using, in case the next prefix matches
                        // more owned chunks.
                        _seekPoint.keyPrefix = kv->key;
                        _seekPoint.prefixLen = _fieldNo + 1;
                        _seekPoint.firstExclusive = _fieldNo;
                        _workingSet->free(id);
                        return PlanStage::NEED_TIME;
                    }
                    case mongo::OrphanChunkSkipper::NoMoreOwned: {
                        // We're done! No more owned chunks remain, as the shard key is a contiguous
                        // prefix of the current index.
                        _workingSet->free(id);
                        // We also count this as a chunk-skip, since there may have been additional
                        // orphan values after the previous one (in any case, we end early).
                        ++_specificStats.orphanChunkSkips;
                        return PlanStage::IS_EOF;
                    }
                    default:
                        MONGO_UNREACHABLE_TASSERT(9246503);
                }
            } else if (_shardFilterer) {
                // We need one last check before we can return the key if we've been initialized
                // with a shard filter. If this document is an orphan, we need to try the next one;
                // otherwise, we can proceed with a regular distinct scan.
                belongs = _shardFilterer->documentBelongsToMe(*member);
            }

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
                    // If we're here, we found an orphan and have no orphan chunk skipper; we need
                    // to try the next entry in the index until we find the next non-orphan, and we
                    // can resume seeking to the next distinct value.
                    _needsSequentialScan = true;
                    _workingSet->free(id);
                    // Similar to shard-filtering, increment the counter for individual orphan docs
                    // we've filtered out via the shard-filter.
                    ++_specificStats.chunkSkips;
                    return PlanStage::NEED_TIME;
                }
                default:
                    MONGO_UNREACHABLE_TASSERT(9245301);
            }
        }
    }
    MONGO_UNREACHABLE_TASSERT(9245303);
}

bool DistinctScan::isEOF() const {
    return _commonStats.isEOF;
}

void DistinctScan::doSaveStateRequiresIndex() {
    if (_cursor && !_needsSequentialScan) {
        // Unless we are 1) shard filtering, 2) not using an orphan chunk skipper, and 3) scanning
        // past orphans, we always seek, so we don't care where the cursor is.
        _cursor->saveUnpositioned();
    } else if (_cursor) {
        // We are scanning past orphans; save the cursor position.
        _cursor->save();
    }

    if (_fetchCursor) {
        _fetchCursor->saveUnpositioned();
    }
}

void DistinctScan::doRestoreStateRequiresIndex() {
    auto& ru = *shard_role_details::getRecoveryUnit(opCtx());
    if (_cursor) {
        _cursor->restore(ru);
    }

    if (_fetchCursor) {
        uassert(9623400,
                "Could not restore collection cursor for fetching DISTINCT_SCAN",
                _fetchCursor->restore(ru));
    }
}

void DistinctScan::doDetachFromOperationContext() {
    if (_cursor) {
        _cursor->detachFromOperationContext();
    }
    if (_fetchCursor) {
        _fetchCursor->detachFromOperationContext();
    }
}

void DistinctScan::doReattachToOperationContext() {
    if (_cursor) {
        _cursor->reattachToOperationContext(opCtx());
    }
    if (_fetchCursor) {
        _fetchCursor->reattachToOperationContext(opCtx());
    }
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
