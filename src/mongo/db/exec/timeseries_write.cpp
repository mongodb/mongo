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

#include "mongo/db/exec/timeseries_write.h"

#include "mongo/db/timeseries/timeseries_write_util.h"

namespace mongo {

const char* TimeseriesWriteStage::kStageType = "TS_WRITE";

TimeseriesWriteStage::TimeseriesWriteStage(ExpressionContext* expCtx,
                                           WorkingSet* ws,
                                           std::unique_ptr<PlanStage> child,
                                           const CollectionPtr& coll,
                                           std::unique_ptr<MatchExpression> residualPredicate)
    : RequiresCollectionStage(kStageType, expCtx, coll),
      _ws(ws),
      _residualPredicate(std::move(residualPredicate)) {
    _children.emplace_back(std::move(child));
}

bool TimeseriesWriteStage::isEOF() {
    return child()->isEOF();
}

std::unique_ptr<PlanStageStats> TimeseriesWriteStage::getStats() {
    _commonStats.isEOF = isEOF();
    auto ret = std::make_unique<PlanStageStats>(_commonStats, stageType());
    ret->specific = std::make_unique<TimeseriesWriteStats>(_specificStats);
    for (const auto& child : _children) {
        ret->children.emplace_back(child->getStats());
    }
    return ret;
}

void TimeseriesWriteStage::_writeToTimeseriesBuckets() {
    ON_BLOCK_EXIT([&] {
        _specificStats.measurementsDeleted += _deletedMeasurements.size();
        _deletedMeasurements.clear();
        _unchangedMeasurements.clear();
    });

    // No measurements needed to be deleted from the bucket document.
    if (_deletedMeasurements.empty()) {
        return;
    }

    OID bucketId = record_id_helpers::toBSONAs(_currentBucketRid, "_id")["_id"].OID();
    if (_unchangedMeasurements.empty()) {
        write_ops::DeleteOpEntry deleteEntry(BSON("_id" << bucketId), false);
        write_ops::DeleteCommandRequest op(collection()->ns(), {deleteEntry});
        // TODO (SERVER-73093): Handles the write failures through retry.
        auto result = timeseries::performAtomicWrites(opCtx(), collection(), _currentBucketRid, op);
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
        auto result = timeseries::performAtomicWrites(opCtx(), collection(), _currentBucketRid, op);
    }
}

PlanStage::StageState TimeseriesWriteStage::doWork(WorkingSetID* out) {
    if (isEOF()) {
        return PlanStage::IS_EOF;
    }

    WorkingSetID id;
    auto status = child()->work(&id);

    switch (status) {
        case PlanStage::ADVANCED:
            break;
        case PlanStage::IS_EOF:
            // Perform writes for the last bucket document.
            _writeToTimeseriesBuckets();
            return PlanStage::NEED_TIME;
        case PlanStage::NEED_YIELD:
            *out = id;
            return status;
        case PlanStage::NEED_TIME:
            return status;
    }

    auto member = _ws->get(id);
    invariant(member->hasRecordId());
    auto bucketRid = member->recordId;

    // Set to the first bucket's RecordId.
    if (_currentBucketRid.isNull()) {
        _currentBucketRid = bucketRid;
    }

    // The current bucket is exhausted and we will need to perform an atomic write to modify the
    // bucket.
    if (_currentBucketRid != bucketRid) {
        _writeToTimeseriesBuckets();
        _currentBucketRid = bucketRid;
    }

    auto measurement = member->doc.value().toBson().getOwned();
    if (_residualPredicate->matchesBSON(measurement)) {
        _deletedMeasurements.push_back(measurement);
    } else {
        _unchangedMeasurements.push_back(measurement);
    }

    return PlanStage::NEED_TIME;
}
}  // namespace mongo
