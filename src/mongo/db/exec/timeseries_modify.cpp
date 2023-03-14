/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/exec/timeseries_modify.h"

#include "mongo/db/query/plan_executor_impl.h"
#include "mongo/db/timeseries/timeseries_write_util.h"

namespace mongo {

const char* TimeseriesModifyStage::kStageType = "TS_MODIFY";

TimeseriesModifyStage::TimeseriesModifyStage(ExpressionContext* expCtx,
                                             std::unique_ptr<DeleteStageParams> params,
                                             WorkingSet* ws,
                                             std::unique_ptr<PlanStage> child,
                                             const CollectionPtr& coll,
                                             BucketUnpacker bucketUnpacker,
                                             std::unique_ptr<MatchExpression> residualPredicate)
    : RequiresCollectionStage(kStageType, expCtx, coll),
      _params(std::move(params)),
      _ws(ws),
      _bucketUnpacker{std::move(bucketUnpacker)},
      _residualPredicate(std::move(residualPredicate)),
      _preWriteFilter(opCtx(), coll->ns()) {
    _children.emplace_back(std::move(child));
}

bool TimeseriesModifyStage::isEOF() {
    return !_bucketUnpacker.hasNext() && child()->isEOF() &&
        _retryFetchBucketId == WorkingSet::INVALID_ID;
}

std::unique_ptr<PlanStageStats> TimeseriesModifyStage::getStats() {
    _commonStats.isEOF = isEOF();
    auto ret = std::make_unique<PlanStageStats>(_commonStats, stageType());
    ret->specific = std::make_unique<TimeseriesModifyStats>(_specificStats);
    for (const auto& child : _children) {
        ret->children.emplace_back(child->getStats());
    }
    return ret;
}

void TimeseriesModifyStage::_writeToTimeseriesBuckets() {
    ON_BLOCK_EXIT([&] {
        _specificStats.measurementsDeleted += _deletedMeasurements.size();
        _deletedMeasurements.clear();
        _unchangedMeasurements.clear();
        _currentBucketFromMigrate = false;
    });

    if (_params->isExplain) {
        return;
    }

    // No measurements needed to be deleted from the bucket document.
    if (_deletedMeasurements.empty()) {
        return;
    }

    OID bucketId = record_id_helpers::toBSONAs(_currentBucketRid, "_id")["_id"].OID();
    if (_unchangedMeasurements.empty()) {
        write_ops::DeleteOpEntry deleteEntry(BSON("_id" << bucketId), false);
        write_ops::DeleteCommandRequest op(collection()->ns(), {deleteEntry});
        // TODO (SERVER-73093): Handles the write failures through retry.
        auto result = timeseries::performAtomicWrites(
            opCtx(), collection(), _currentBucketRid, op, _currentBucketFromMigrate);
    } else {
        auto timeseriesOptions = collection()->getTimeseriesOptions();
        auto metaFieldName = timeseriesOptions->getMetaField();
        auto metadata =
            metaFieldName ? _unchangedMeasurements[0].getField(*metaFieldName).wrap() : BSONObj();
        auto replaceBucket =
            timeseries::makeNewDocumentForWrite(bucketId,
                                                _unchangedMeasurements,
                                                metadata,
                                                timeseriesOptions,
                                                collection()->getDefaultCollator());

        write_ops::UpdateModification u(replaceBucket);
        write_ops::UpdateOpEntry updateEntry(BSON("_id" << bucketId), std::move(u));
        write_ops::UpdateCommandRequest op(collection()->ns(), {updateEntry});
        // TODO (SERVER-73093): Handles the write failures through retry.
        auto result = timeseries::performAtomicWrites(
            opCtx(), collection(), _currentBucketRid, op, _currentBucketFromMigrate);
    }
}

boost::optional<PlanStage::StageState> TimeseriesModifyStage::rememberIfWritingToOrphanedBucket(
    WorkingSetMember* member) {
    // If we are in explain mode, we do not need to check if the bucket is orphaned since we're not
    // writing to bucket. If we are migrating a bucket, we also do not need to check if the bucket
    // is not writable and just remember it.
    if (_params->isExplain || _params->fromMigrate) {
        _currentBucketFromMigrate = _params->fromMigrate;
        return boost::none;
    }

    auto [immediateReturnStageState, currentBucketFromMigrate] =
        _preWriteFilter.checkIfNotWritable(member->doc.value(),
                                           "timeseriesDelete"_sd,
                                           collection()->ns(),
                                           [&](const ExceptionFor<ErrorCodes::StaleConfig>& ex) {
                                               planExecutorShardingCriticalSectionFuture(opCtx()) =
                                                   ex->getCriticalSectionSignal();
                                               // TODO (SERVER-73093): Retry the write if we're in
                                               // the sharding critical section.
                                           });

    // We need to immediately return if the bucket is orphaned or we're in the sharding critical
    // section and hence should yield.
    if (immediateReturnStageState) {
        return *immediateReturnStageState;
    }

    _currentBucketFromMigrate = currentBucketFromMigrate;

    return boost::none;
}

PlanStage::StageState TimeseriesModifyStage::_fetchBucket(WorkingSetID id) {
    return handlePlanStageYield(
        expCtx(),
        "TimeseriesModifyStage::_fetchBucket",
        collection()->ns().ns(),
        [&] {
            auto cursor = collection()->getCursor(opCtx());
            if (!WorkingSetCommon::fetch(
                    opCtx(), _ws, id, cursor.get(), collection(), collection()->ns())) {
                return PlanStage::NEED_TIME;
            }
            return PlanStage::ADVANCED;
        },
        [&] { _retryFetchBucketId = id; });
}

PlanStage::StageState TimeseriesModifyStage::_getNextBucket(WorkingSetID& id) {
    if (_retryFetchBucketId == WorkingSet::INVALID_ID) {
        auto status = child()->work(&id);
        if (status != PlanStage::ADVANCED) {
            return status;
        }

        auto member = _ws->get(id);
        // TODO SERVER-73142 remove this assert, we may not have an object if we have a spool child.
        tassert(7443600, "Child should have provided the whole document", member->hasObj());
        if (member->hasObj()) {
            // We already got the whole document from our child, no need to fetch.
            return PlanStage::ADVANCED;
        }
    } else {
        // We have a bucket that we need to fetch before we can unpack it.
        id = _retryFetchBucketId;
        _retryFetchBucketId = WorkingSet::INVALID_ID;
    }

    // The result returned from our child had only a RecordId, no document, so we need to fetch it.
    // This is the case when we have a spool child.
    return _fetchBucket(id);
}

PlanStage::StageState TimeseriesModifyStage::doWork(WorkingSetID* out) {
    if (isEOF()) {
        return PlanStage::IS_EOF;
    }

    // The current bucket is exhausted. Perform an atomic write to modify the bucket and move on to
    // the next.
    if (!_bucketUnpacker.hasNext()) {
        _writeToTimeseriesBuckets();

        auto id = WorkingSet::INVALID_ID;
        auto status = _getNextBucket(id);

        if (PlanStage::ADVANCED == status) {
            // We want to free this member when we return because we either have an owned copy of
            // the bucket for normal write and write to orphan cases, or we skip the bucket, or we
            // don't retry as of now.
            // TODO (SERVER-73093): Need to dismiss 'memberFreer' if we're going to retry the write.
            ScopeGuard memberFreer([&] { _ws->free(id); });

            auto member = _ws->get(id);
            tassert(7459100, "Expected a RecordId from the child stage", member->hasRecordId());

            if (auto immediateReturnStageState = rememberIfWritingToOrphanedBucket(member)) {
                return *immediateReturnStageState;
            }
            _currentBucketRid = member->recordId;

            // Make an owned copy of the bucket document if necessary. The bucket will be
            // unwound across multiple calls to 'doWork()', so we need to hold our own copy in
            // the query execution layer in case the storage engine reclaims the memory for the
            // bucket between calls to 'doWork()'.
            auto ownedBucket = member->doc.value().toBson().getOwned();
            _bucketUnpacker.reset(std::move(ownedBucket));
            ++_specificStats.bucketsUnpacked;
        } else {
            if (PlanStage::NEED_YIELD == status) {
                *out = id;
            }
            return status;
        }
    }

    invariant(_bucketUnpacker.hasNext());
    auto measurement = _bucketUnpacker.getNext().toBson().getOwned();
    if (_residualPredicate->matchesBSON(measurement)) {
        _deletedMeasurements.push_back(measurement);
    } else {
        _unchangedMeasurements.push_back(measurement);
    }

    return PlanStage::NEED_TIME;
}

void TimeseriesModifyStage::doRestoreStateRequiresCollection() {
    const NamespaceString& ns = collection()->ns();
    uassert(ErrorCodes::PrimarySteppedDown,
            "Demoted from primary while removing from {}"_format(ns.ns()),
            !opCtx()->writesAreReplicated() ||
                repl::ReplicationCoordinator::get(opCtx())->canAcceptWritesFor(opCtx(), ns));

    _preWriteFilter.restoreState();
}
}  // namespace mongo
