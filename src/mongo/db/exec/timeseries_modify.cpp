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
    _children.emplace_back(std::move(child));
}

bool TimeseriesModifyStage::isEOF() {
    return !_bucketUnpacker.hasNext() && child()->isEOF() &&
        _retryBucketId == WorkingSet::INVALID_ID;
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

void TimeseriesModifyStage::resetCurrentBucket() {
    _deletedMeasurements.clear();
    _unchangedMeasurements.clear();
    _currentBucketFromMigrate = false;
    _currentBucketRid = RecordId{};
    _currentBucketSnapshotId = SnapshotId{};
}

PlanStage::StageState TimeseriesModifyStage::_writeToTimeseriesBuckets() {
    ON_BLOCK_EXIT([&] { resetCurrentBucket(); });

    if (_params->isExplain) {
        _specificStats.measurementsDeleted += _deletedMeasurements.size();
        return PlanStage::NEED_TIME;
    }

    // No measurements needed to be deleted from the bucket document.
    if (_deletedMeasurements.empty()) {
        return PlanStage::NEED_TIME;
    }

    if (opCtx()->recoveryUnit()->getSnapshotId() != _currentBucketSnapshotId) {
        // The snapshot has changed, so we have no way to prove that the bucket we're
        // unwinding still exists in the same shape it did originally. If it has changed, we
        // risk re-inserting a measurement that we can see in our cache but which has
        // actually since been deleted. So we have to fetch and retry this bucket.
        _retryBucket(_currentBucketRid);
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

    OID bucketId = record_id_helpers::toBSONAs(_currentBucketRid, "_id")["_id"].OID();
    if (_unchangedMeasurements.empty()) {
        write_ops::DeleteOpEntry deleteEntry(BSON("_id" << bucketId), false);
        write_ops::DeleteCommandRequest op(collection()->ns(), {deleteEntry});

        auto result = timeseries::performAtomicWrites(
            opCtx(), collection(), _currentBucketRid, op, _currentBucketFromMigrate);
        if (!result.isOK()) {
            LOGV2_DEBUG(7309300,
                        5,
                        "Retrying bucket due to conflict attempting to write out changes",
                        "bucket_rid"_attr = _currentBucketRid);
            _retryBucket(_currentBucketRid);
            return PlanStage::NEED_YIELD;
        }
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

        auto result = timeseries::performAtomicWrites(
            opCtx(), collection(), _currentBucketRid, op, _currentBucketFromMigrate);
        if (!result.isOK()) {
            LOGV2_DEBUG(7309301,
                        5,
                        "Retrying bucket due to conflict attempting to write out changes",
                        "bucket_rid"_attr = _currentBucketRid);
            _retryBucket(_currentBucketRid);
            return PlanStage::NEED_YIELD;
        }
    }
    _specificStats.measurementsDeleted += _deletedMeasurements.size();

    // As restoreState may restore (recreate) cursors, cursors are tied to the transaction in
    // which they are created, and a WriteUnitOfWork is a transaction, make sure to restore the
    // state outside of the WriteUnitOfWork.
    return handlePlanStageYield(
        expCtx(),
        "TimeseriesModifyStage restoreState",
        collection()->ns().ns(),
        [&] {
            child()->restoreState(&collection());
            return PlanStage::NEED_TIME;
        },
        // yieldHandler
        // Note we don't need to retry anything in this case since the
        // delete already was committed. However, we still need to
        // return the deleted document (if it was requested).
        // TODO for findAndModify we need to return the deleted doc.
        [&] { /* noop */ });
}

template <typename F>
boost::optional<PlanStage::StageState> TimeseriesModifyStage::_rememberIfWritingToOrphanedBucket(
    ScopeGuard<F>& bucketFreer, WorkingSetID id) {
    // If we are in explain mode, we do not need to check if the bucket is orphaned since we're not
    // writing to bucket. If we are migrating a bucket, we also do not need to check if the bucket
    // is not writable and just remember it.
    if (_params->isExplain || _params->fromMigrate) {
        _currentBucketFromMigrate = _params->fromMigrate;
        return boost::none;
    }

    auto [immediateReturnStageState, currentBucketFromMigrate] =
        _preWriteFilter.checkIfNotWritable(_ws->get(id)->doc.value(),
                                           "timeseriesDelete"_sd,
                                           collection()->ns(),
                                           [&](const ExceptionFor<ErrorCodes::StaleConfig>& ex) {
                                               planExecutorShardingCriticalSectionFuture(opCtx()) =
                                                   ex->getCriticalSectionSignal();
                                               // Retry the write if we're in the sharding critical
                                               // section.
                                               bucketFreer.dismiss();
                                               _retryBucket(id);
                                           });

    // We need to immediately return if the bucket is orphaned or we're in the sharding critical
    // section and hence should yield.
    if (immediateReturnStageState) {
        return *immediateReturnStageState;
    }

    _currentBucketFromMigrate = currentBucketFromMigrate;

    return boost::none;
}

PlanStage::StageState TimeseriesModifyStage::_getNextBucket(WorkingSetID& id) {
    if (_retryBucketId == WorkingSet::INVALID_ID) {
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
        id = _retryBucketId;
        _retryBucketId = WorkingSet::INVALID_ID;
    }

    // We don't have an up-to-date document for this RecordId. Fetch it and ensure that it still
    // exists and matches our predicate.
    bool docStillMatches;

    const auto ret = handlePlanStageYield(
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
    if (ret != PlanStage::NEED_TIME) {
        return ret;
    }

    return docStillMatches ? PlanStage::ADVANCED : PlanStage::NEED_TIME;
}

void TimeseriesModifyStage::_retryBucket(const stdx::variant<WorkingSetID, RecordId>& bucketId) {
    tassert(7309302,
            "Cannot be in the middle of unpacking a bucket if retrying",
            !_bucketUnpacker.hasNext());
    tassert(7309303,
            "Cannot retry two buckets at the same time",
            _retryBucketId == WorkingSet::INVALID_ID);

    stdx::visit(OverloadedVisitor{
                    [&](WorkingSetID id) { _retryBucketId = id; },
                    [&](const RecordId& rid) {
                        // We don't have a working set member referencing this bucket, allocate one.
                        _retryBucketId = _ws->allocate();
                        auto member = _ws->get(_retryBucketId);
                        member->recordId = rid;
                        member->doc.setSnapshotId(_currentBucketSnapshotId);
                        member->transitionToRecordIdAndObj();
                    },
                },
                bucketId);

    resetCurrentBucket();
}

PlanStage::StageState TimeseriesModifyStage::doWork(WorkingSetID* out) {
    if (isEOF()) {
        return PlanStage::IS_EOF;
    }

    // The current bucket is exhausted. Perform an atomic write to modify the bucket and move on to
    // the next.
    if (!_bucketUnpacker.hasNext()) {
        auto status = _writeToTimeseriesBuckets();
        if (status != PlanStage::NEED_TIME) {
            *out = WorkingSet::INVALID_ID;
            return status;
        }

        auto id = WorkingSet::INVALID_ID;
        status = _getNextBucket(id);

        if (PlanStage::ADVANCED == status) {
            // We want to free this member when we return because we either have an owned copy
            // of the bucket for normal write and write to orphan cases, or we skip the bucket.
            ScopeGuard bucketFreer([&] { _ws->free(id); });

            auto member = _ws->get(id);
            tassert(7459100, "Expected a RecordId from the child stage", member->hasRecordId());

            if (auto immediateReturnStageState =
                    _rememberIfWritingToOrphanedBucket(bucketFreer, id)) {
                return *immediateReturnStageState;
            }

            tassert(7309304,
                    "Expected no bucket to retry after getting a new bucket",
                    _retryBucketId == WorkingSet::INVALID_ID);
            _currentBucketRid = member->recordId;
            _currentBucketSnapshotId = member->doc.snapshotId();

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
