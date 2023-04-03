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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kWrite

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
    tassert(7308200,
            "multi is true and no residual predicate was specified",
            _isDeleteOne() || _residualPredicate);
    _children.emplace_back(std::move(child));

    // These three properties are only used for the queryPlanner explain and will not change while
    // executing this stage.
    _specificStats.opType = [&] {
        if (_isDeleteOne()) {
            return "deleteOne";
        } else {
            return "deleteMany";
        }
    }();
    _specificStats.bucketFilter = _params->canonicalQuery->getQueryObj();
    if (_residualPredicate) {
        _specificStats.residualFilter = _residualPredicate->serialize();
    }
}

bool TimeseriesModifyStage::isEOF() {
    if (_isDeleteOne() && _specificStats.nMeasurementsDeleted > 0) {
        return true;
    }
    return child()->isEOF() && _retryBucketId == WorkingSet::INVALID_ID;
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

PlanStage::StageState TimeseriesModifyStage::_writeToTimeseriesBuckets(
    WorkingSetID bucketWsmId,
    const std::vector<BSONObj>& unchangedMeasurements,
    const std::vector<BSONObj>& deletedMeasurements,
    bool bucketFromMigrate) {
    if (_params->isExplain) {
        _specificStats.nMeasurementsDeleted += deletedMeasurements.size();
        return PlanStage::NEED_TIME;
    }

    // No measurements needed to be deleted from the bucket document.
    if (deletedMeasurements.empty()) {
        return PlanStage::NEED_TIME;
    }

    handlePlanStageYield(
        expCtx(),
        "TimeseriesModifyStage saveState",
        collection()->ns().ns(),
        [&] {
            child()->saveState();
            return PlanStage::NEED_TIME /* unused */;
        },
        [&] {
            // yieldHandler
            std::terminate();
        });

    auto recordId = _ws->get(bucketWsmId)->recordId;

    auto yieldAndRetry = [&](unsigned logId) {
        LOGV2_DEBUG(logId,
                    5,
                    "Retrying bucket due to conflict attempting to write out changes",
                    "bucket_rid"_attr = recordId);
        _retryBucket(bucketWsmId);
        return PlanStage::NEED_YIELD;
    };

    OID bucketId = record_id_helpers::toBSONAs(recordId, "_id")["_id"].OID();
    if (unchangedMeasurements.empty()) {
        write_ops::DeleteOpEntry deleteEntry(BSON("_id" << bucketId), false);
        write_ops::DeleteCommandRequest op(collection()->ns(), {deleteEntry});

        auto result = timeseries::performAtomicWrites(
            opCtx(), collection(), recordId, op, bucketFromMigrate, _params->stmtId);
        if (!result.isOK()) {
            return yieldAndRetry(7309300);
        }
    } else {
        auto timeseriesOptions = collection()->getTimeseriesOptions();
        auto metaFieldName = timeseriesOptions->getMetaField();
        auto metadata = [&] {
            if (!metaFieldName) {  // Collection has no metadata field.
                return BSONObj();
            }
            // Look for the metadata field on this bucket and return it if present.
            auto metaField = unchangedMeasurements[0].getField(*metaFieldName);
            return metaField ? metaField.wrap() : BSONObj();
        }();
        auto replaceBucket =
            timeseries::makeNewDocumentForWrite(bucketId,
                                                unchangedMeasurements,
                                                metadata,
                                                timeseriesOptions,
                                                collection()->getDefaultCollator());

        write_ops::UpdateModification u(replaceBucket);
        write_ops::UpdateOpEntry updateEntry(BSON("_id" << bucketId), std::move(u));
        write_ops::UpdateCommandRequest op(collection()->ns(), {updateEntry});

        auto result = timeseries::performAtomicWrites(
            opCtx(), collection(), recordId, op, bucketFromMigrate, _params->stmtId);
        if (!result.isOK()) {
            return yieldAndRetry(7309301);
        }
    }
    _specificStats.nMeasurementsDeleted += deletedMeasurements.size();

    // As restoreState may restore (recreate) cursors, cursors are tied to the
    // transaction in which they are created, and a WriteUnitOfWork is a transaction,
    // make sure to restore the state outside of the WriteUnitOfWork.
    return handlePlanStageYield(
        expCtx(),
        "TimeseriesModifyStage restoreState",
        collection()->ns().ns(),
        [&] {
            child()->restoreState(&collection());
            return PlanStage::NEED_TIME;
        },
        // yieldHandler
        // Note we don't need to retry anything in this case since the delete already
        // was committed. However, we still need to return the deleted document (if it
        // was requested).
        // TODO SERVER-73089 for findAndModify we need to return the deleted doc.
        [&] { /* noop */ });
}

template <typename F>
std::pair<boost::optional<PlanStage::StageState>, bool>
TimeseriesModifyStage::_checkIfWritingToOrphanedBucket(ScopeGuard<F>& bucketFreer,
                                                       WorkingSetID id) {
    // If we are in explain mode, we do not need to check if the bucket is orphaned since
    // we're not writing to bucket. If we are migrating a bucket, we also do not need to
    // check if the bucket is not writable and just return it.
    if (_params->isExplain || _params->fromMigrate) {
        return {boost::none, _params->fromMigrate};
    }
    return _preWriteFilter.checkIfNotWritable(_ws->get(id)->doc.value(),
                                              "timeseriesDelete"_sd,
                                              collection()->ns(),
                                              [&](const ExceptionFor<ErrorCodes::StaleConfig>& ex) {
                                                  planExecutorShardingCriticalSectionFuture(
                                                      opCtx()) = ex->getCriticalSectionSignal();
                                                  // Retry the write if we're in the sharding
                                                  // critical section.
                                                  bucketFreer.dismiss();
                                                  _retryBucket(id);
                                              });
}

PlanStage::StageState TimeseriesModifyStage::_getNextBucket(WorkingSetID& id) {
    if (_retryBucketId == WorkingSet::INVALID_ID) {
        auto status = child()->work(&id);
        if (status != PlanStage::ADVANCED) {
            return status;
        }
    } else {
        id = _retryBucketId;
        _retryBucketId = WorkingSet::INVALID_ID;
    }

    // We may not have an up-to-date bucket for this RecordId. Fetch it and ensure that it
    // still exists and matches our bucket-level predicate if it is not believed to be
    // up-to-date.
    bool docStillMatches;

    const auto status = handlePlanStageYield(
        expCtx(),
        "TimeseriesModifyStage:: ensureStillMatches",
        collection()->ns().ns(),
        [&] {
            docStillMatches = write_stage_common::ensureStillMatches(
                collection(), opCtx(), _ws, id, _params->canonicalQuery);
            return PlanStage::NEED_TIME;
        },
        [&] {
            // yieldHandler
            // There was a problem trying to detect if the document still exists, so retry.
            _retryBucket(id);
        });
    if (status != PlanStage::NEED_TIME) {
        return status;
    }
    return docStillMatches ? PlanStage::ADVANCED : PlanStage::NEED_TIME;
}

void TimeseriesModifyStage::_retryBucket(WorkingSetID bucketId) {
    tassert(7309302,
            "Cannot be in the middle of unpacking a bucket if retrying",
            !_bucketUnpacker.hasNext());
    tassert(7309303,
            "Cannot retry two buckets at the same time",
            _retryBucketId == WorkingSet::INVALID_ID);

    _retryBucketId = bucketId;
}

PlanStage::StageState TimeseriesModifyStage::doWork(WorkingSetID* out) {
    if (isEOF()) {
        return PlanStage::IS_EOF;
    }

    tassert(7495500,
            "Expected bucketUnpacker's current bucket to be exhausted",
            !_bucketUnpacker.hasNext() || _bucketUnpacker.isClosedBucket());

    auto id = WorkingSet::INVALID_ID;
    auto status = _getNextBucket(id);
    if (status != PlanStage::ADVANCED) {
        if (status == PlanStage::NEED_YIELD) {
            *out = id;
        }
        return status;
    }

    // We want to free this member when we return because we either have an owned copy of
    // the bucket for normal write and write to orphan cases, or we skip the bucket.
    ScopeGuard bucketFreer([&] { _ws->free(id); });

    auto member = _ws->get(id);
    tassert(7459100, "Expected a RecordId from the child stage", member->hasRecordId());

    // Determine if we are writing to an orphaned bucket - such writes should be excluded
    // from user-visible change stream events. This will be achieved later by setting
    // 'fromMigrate' flag when calling performAtomicWrites().
    auto [immediateReturnStageState, bucketFromMigrate] =
        _checkIfWritingToOrphanedBucket(bucketFreer, id);
    if (immediateReturnStageState) {
        return *immediateReturnStageState;
    }
    tassert(7309304,
            "Expected no bucket to retry after getting a new bucket",
            _retryBucketId == WorkingSet::INVALID_ID);

    // Unpack the bucket and determine which measurements match the residual predicate.
    auto ownedBucket = member->doc.value().toBson().getOwned();
    _bucketUnpacker.reset(std::move(ownedBucket));
    if (_bucketUnpacker.isClosedBucket()) {
        // This bucket is closed, skip it.
        return PlanStage::NEED_TIME;
    }
    ++_specificStats.nBucketsUnpacked;

    std::vector<BSONObj> unchangedMeasurements;
    std::vector<BSONObj> deletedMeasurements;

    while (_bucketUnpacker.hasNext()) {
        auto measurement = _bucketUnpacker.getNext().toBson();
        // We should stop deleting measurements once we hit the limit of one in the not multi case.
        bool shouldContinueDeleting = _isDeleteMulti() || deletedMeasurements.empty();
        if (shouldContinueDeleting &&
            (!_residualPredicate || _residualPredicate->matchesBSON(measurement))) {
            deletedMeasurements.push_back(measurement);
        } else {
            unchangedMeasurements.push_back(measurement);
        }
    }

    status = _writeToTimeseriesBuckets(
        id, unchangedMeasurements, deletedMeasurements, bucketFromMigrate);
    if (status != PlanStage::NEED_TIME) {
        *out = WorkingSet::INVALID_ID;
        bucketFreer.dismiss();
    }
    return status;
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
