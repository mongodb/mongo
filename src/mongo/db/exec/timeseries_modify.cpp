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

#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/query/plan_executor_impl.h"
#include "mongo/db/timeseries/timeseries_write_util.h"
#include "mongo/db/update/update_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kWrite

namespace mongo {

const char* TimeseriesModifyStage::kStageType = "TS_MODIFY";

TimeseriesModifyStage::TimeseriesModifyStage(ExpressionContext* expCtx,
                                             TimeseriesModifyParams&& params,
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
    uassert(ErrorCodes::InvalidOptions, "Arbitrary updates not yet enabled", !_params.isUpdate);
    tassert(7308200,
            "Multi deletes must have a residual predicate",
            _isSingletonWrite() || _residualPredicate || _params.isUpdate);
    tassert(7308300,
            "Can return the deleted measurement only if deleting one",
            !_params.returnDeleted || _isSingletonWrite());
    _children.emplace_back(std::move(child));

    // These three properties are only used for the queryPlanner explain and will not change while
    // executing this stage.
    _specificStats.opType = [&] {
        if (_params.isUpdate) {
            return _isMultiWrite() ? "updateMany" : "updateOne";
        }
        return _isMultiWrite() ? "deleteMany" : "deleteOne";
    }();
    _specificStats.bucketFilter = _params.canonicalQuery->getQueryObj();
    if (_residualPredicate) {
        _specificStats.residualFilter = _residualPredicate->serialize();
    }

    tassert(7314202,
            "Updates must specify an update driver",
            _params.updateDriver || !_params.isUpdate);
    _specificStats.isModUpdate =
        _params.isUpdate && _params.updateDriver->type() == UpdateDriver::UpdateType::kOperator;

    // TODO SERVER-73143 Enable these cases.
    uassert(ErrorCodes::InvalidOptions,
            "Timeseries arbitrary updates must be modifier updates",
            !_specificStats.isModUpdate);
}

bool TimeseriesModifyStage::isEOF() {
    if (_isSingletonWrite() && _specificStats.nMeasurementsModified > 0) {
        // If we have a measurement to return, we should not return EOF so that we can get a chance
        // to get called again and return the measurement.
        return !_deletedMeasurementToReturn;
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

template <typename F>
std::pair<bool, PlanStage::StageState> TimeseriesModifyStage::_writeToTimeseriesBuckets(
    ScopeGuard<F>& bucketFreer,
    WorkingSetID bucketWsmId,
    const std::vector<BSONObj>& unchangedMeasurements,
    const std::vector<BSONObj>& modifiedMeasurements,
    bool bucketFromMigrate) {
    // No measurements needed to be deleted from the bucket document.
    if (modifiedMeasurements.empty()) {
        return {false, PlanStage::NEED_TIME};
    }

    // We don't actually write anything if we are in explain mode but we still need to update the
    // stats and let the caller think as if the write succeeded if there's any deleted measurement.
    if (_params.isExplain) {
        _specificStats.nMeasurementsModified += modifiedMeasurements.size();
        _specificStats.nMeasurementsMatched += modifiedMeasurements.size();
        return {true, PlanStage::NEED_TIME};
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

    auto yieldAndRetry = [&](unsigned logId, Status status) {
        LOGV2_DEBUG(logId,
                    5,
                    "Retrying bucket due to conflict attempting to write out changes",
                    "bucket_rid"_attr = recordId,
                    "status"_attr = status);
        // We need to retry the bucket, so we should not free the current bucket.
        bucketFreer.dismiss();
        _retryBucket(bucketWsmId);
        return PlanStage::NEED_YIELD;
    };

    OID bucketId = record_id_helpers::toBSONAs(recordId, "_id")["_id"].OID();
    if (unchangedMeasurements.empty()) {
        write_ops::DeleteOpEntry deleteEntry(BSON("_id" << bucketId), false);
        write_ops::DeleteCommandRequest op(collection()->ns(), {deleteEntry});

        auto result = timeseries::performAtomicWrites(
            opCtx(), collection(), recordId, op, {}, bucketFromMigrate, _params.stmtId);
        if (!result.isOK()) {
            return {false, yieldAndRetry(7309300, result)};
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
            opCtx(), collection(), recordId, op, {}, bucketFromMigrate, _params.stmtId);
        if (!result.isOK()) {
            return {false, yieldAndRetry(7309301, result)};
        }
    }
    _specificStats.nMeasurementsMatched += modifiedMeasurements.size();
    _specificStats.nMeasurementsModified += modifiedMeasurements.size();

    // As restoreState may restore (recreate) cursors, cursors are tied to the transaction in which
    // they are created, and a WriteUnitOfWork is a transaction, make sure to restore the state
    // outside of the WriteUnitOfWork.
    auto status = handlePlanStageYield(
        expCtx(),
        "TimeseriesModifyStage restoreState",
        collection()->ns().ns(),
        [&] {
            child()->restoreState(&collection());
            return PlanStage::NEED_TIME;
        },
        // yieldHandler
        // Note we don't need to retry anything in this case since the write already was committed.
        // However, we still need to return the affected measurement (if it was requested). We don't
        // need to rely on the storage engine to return the affected document since we already have
        // it in memory.
        [&] { /* noop */ });

    return {true, status};
}

template <typename F>
std::pair<boost::optional<PlanStage::StageState>, bool>
TimeseriesModifyStage::_checkIfWritingToOrphanedBucket(ScopeGuard<F>& bucketFreer,
                                                       WorkingSetID id) {
    // If we are in explain mode, we do not need to check if the bucket is orphaned since we're not
    // writing to bucket. If we are migrating a bucket, we also do not need to check if the bucket
    // is not writable and just return it.
    if (_params.isExplain || _params.fromMigrate) {
        return {boost::none, _params.fromMigrate};
    }
    return _preWriteFilter.checkIfNotWritable(_ws->get(id)->doc.value(),
                                              "timeseries "_sd + _specificStats.opType,
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

    // We may not have an up-to-date bucket for this RecordId. Fetch it and ensure that it still
    // exists and matches our bucket-level predicate if it is not believed to be up-to-date.
    bool docStillMatches;

    const auto status = handlePlanStageYield(
        expCtx(),
        "TimeseriesModifyStage:: ensureStillMatches",
        collection()->ns().ns(),
        [&] {
            docStillMatches = write_stage_common::ensureStillMatches(
                collection(), opCtx(), _ws, id, _params.canonicalQuery);
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

void TimeseriesModifyStage::_prepareToReturnDeletedMeasurement(WorkingSetID& out,
                                                               BSONObj measurement) {
    out = _ws->allocate();
    auto member = _ws->get(out);
    // The measurement does not have record id.
    member->recordId = RecordId{};
    member->doc.value() = Document{std::move(measurement)};
    _ws->transitionToOwnedObj(out);
}

PlanStage::StageState TimeseriesModifyStage::doWork(WorkingSetID* out) {
    if (isEOF()) {
        return PlanStage::IS_EOF;
    }

    if (_deletedMeasurementToReturn) {
        // If we fall into this case, then we were asked to return the deleted measurement but we
        // were not able to do so in the previous call to doWork() because we needed to yield. Now
        // that we are back, we can return the deleted measurement.
        _prepareToReturnDeletedMeasurement(*out, *_deletedMeasurementToReturn);
        _deletedMeasurementToReturn.reset();
        return PlanStage::ADVANCED;
    }

    tassert(7495500,
            "Expected bucketUnpacker's current bucket to be exhausted",
            !_bucketUnpacker.hasNext());

    auto id = WorkingSet::INVALID_ID;
    auto status = _getNextBucket(id);
    if (status != PlanStage::ADVANCED) {
        if (status == PlanStage::NEED_YIELD) {
            *out = id;
        }
        return status;
    }

    // We want to free this member when we return because we either have an owned copy of the bucket
    // for normal write and write to orphan cases, or we skip the bucket.
    ScopeGuard bucketFreer([&] { _ws->free(id); });

    auto member = _ws->get(id);
    tassert(7459100, "Expected a RecordId from the child stage", member->hasRecordId());

    // Determine if we are writing to an orphaned bucket - such writes should be excluded from
    // user-visible change stream events. This will be achieved later by setting 'fromMigrate' flag
    // when calling performAtomicWrites().
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
    // Closed buckets should have been filtered out by the bucket predicate.
    tassert(7554700, "Expected bucket to not be closed", !_bucketUnpacker.isClosedBucket());
    ++_specificStats.nBucketsUnpacked;

    std::vector<BSONObj> unchangedMeasurements;
    std::vector<BSONObj> modifiedMeasurements;

    while (_bucketUnpacker.hasNext()) {
        auto measurement = _bucketUnpacker.getNext().toBson();
        // We should stop deleting measurements once we hit the limit of one in the not multi case.
        bool shouldContinueModifying = _isMultiWrite() || modifiedMeasurements.empty();
        if (shouldContinueModifying &&
            (!_residualPredicate || _residualPredicate->matchesBSON(measurement))) {
            modifiedMeasurements.push_back(measurement);
        } else {
            unchangedMeasurements.push_back(measurement);
        }
    }

    auto isWriteSuccessful = false;
    std::tie(isWriteSuccessful, status) = _writeToTimeseriesBuckets(
        bucketFreer, id, unchangedMeasurements, modifiedMeasurements, bucketFromMigrate);
    if (status != PlanStage::NEED_TIME) {
        *out = WorkingSet::INVALID_ID;
        if (_params.returnDeleted && isWriteSuccessful) {
            // If asked to return the deleted measurement and the write was successful but we need
            // to yield, we need to save the deleted measurement to return it later. See isEOF() for
            // more info.
            tassert(7308301,
                    "Can return only one deleted measurement",
                    modifiedMeasurements.size() == 1);
            _deletedMeasurementToReturn = std::move(modifiedMeasurements[0]);
        }
    } else if (_params.returnDeleted && isWriteSuccessful) {
        // If the write was successful and if asked to return the deleted measurement, we return it
        // immediately.
        _prepareToReturnDeletedMeasurement(*out, modifiedMeasurements[0]);
        status = PlanStage::ADVANCED;
    }
    return status;
}

void TimeseriesModifyStage::doRestoreStateRequiresCollection() {
    const NamespaceString& ns = collection()->ns();
    uassert(ErrorCodes::PrimarySteppedDown,
            "Demoted from primary while removing from {}"_format(ns.toStringForErrorMsg()),
            !opCtx()->writesAreReplicated() ||
                repl::ReplicationCoordinator::get(opCtx())->canAcceptWritesFor(opCtx(), ns));

    _preWriteFilter.restoreState();
}
}  // namespace mongo
