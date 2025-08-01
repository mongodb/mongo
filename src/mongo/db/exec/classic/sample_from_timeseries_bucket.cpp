/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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


#include "mongo/db/exec/classic/sample_from_timeseries_bucket.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/client.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/shard_filterer.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/record_id.h"
#include "mongo/db/storage/snapshot.h"
#include "mongo/db/timeseries/timeseries_constants.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/random.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


namespace mongo {
const char* SampleFromTimeseriesBucket::kStageType = "SAMPLE_FROM_TIMESERIES_BUCKET";

SampleFromTimeseriesBucket::SampleFromTimeseriesBucket(
    ExpressionContext* expCtx,
    WorkingSet* ws,
    std::unique_ptr<PlanStage> child,
    timeseries::BucketUnpacker bucketUnpacker,
    boost::optional<std::unique_ptr<ShardFilterer>> shardFilterer,
    int maxConsecutiveAttempts,
    long long sampleSize,
    int bucketMaxCount)
    : PlanStage{kStageType, expCtx},
      _ws{*ws},
      _bucketUnpacker{std::move(bucketUnpacker)},
      _shardFilterer{std::move(shardFilterer)},
      _maxConsecutiveAttempts{maxConsecutiveAttempts},
      _sampleSize{sampleSize},
      _bucketMaxCount{bucketMaxCount} {
    tassert(5521500, "sampleSize must be gte to 0", sampleSize >= 0);
    tassert(5521501, "bucketMaxCount must be gt 0", bucketMaxCount > 0);

    _children.emplace_back(std::move(child));
}

void SampleFromTimeseriesBucket::materializeMeasurement(int64_t measurementIdx,
                                                        WorkingSetMember* member) {
    auto sampledDocument = _bucketUnpacker.extractSingleMeasurement(measurementIdx);

    member->keyData.clear();
    member->recordId = {};
    member->doc = {{}, std::move(sampledDocument)};
    member->transitionToOwnedObj();
}

std::unique_ptr<PlanStageStats> SampleFromTimeseriesBucket::getStats() {
    _commonStats.isEOF = isEOF();
    auto ret = std::make_unique<PlanStageStats>(_commonStats, stageType());
    ret->specific = std::make_unique<SampleFromTimeseriesBucketStats>(_specificStats);
    ret->children.emplace_back(child()->getStats());
    return ret;
}

PlanStage::StageState SampleFromTimeseriesBucket::doWork(WorkingSetID* out) {
    if (isEOF()) {
        return PlanStage::IS_EOF;
    }

    auto id = WorkingSet::INVALID_ID;
    auto status = child()->work(&id);

    if (PlanStage::ADVANCED == status) {
        auto member = _ws.get(id);

        auto bucket = member->doc.value().toBson();
        if (_shardFilterer) {
            const auto belongs = (*_shardFilterer)->documentBelongsToMe(bucket);
            if (belongs == ShardFilterer::DocumentBelongsResult::kNoShardKey) {
                LOGV2_WARNING(
                    5757300,
                    "no shard key found in document {bucket} for shard key "
                    "pattern {shardFilterer_getKeyPattern}, document may have been inserted "
                    "manually into shard",
                    "bucket"_attr = redact(bucket),
                    "shardFilterer_getKeyPattern"_attr = (*_shardFilterer)->getKeyPattern());
            } else if (belongs != ShardFilterer::DocumentBelongsResult::kBelongs) {
                _ws.free(id);
                return PlanStage::NEED_TIME;
            }
        }

        _bucketUnpacker.reset(std::move(bucket));

        auto& prng = expCtx()->getOperationContext()->getClient()->getPrng();
        auto j = prng.nextInt64(_bucketMaxCount);

        if (j < _bucketUnpacker.numberOfMeasurements()) {
            auto bucketId = _bucketUnpacker.bucket()[timeseries::kBucketIdFieldName];
            auto bucketIdMeasurementIdxKey = SampledMeasurementKey{bucketId.OID(), j};

            ++_specificStats.dupsTested;
            if (_seenSet.insert(std::move(bucketIdMeasurementIdxKey)).second) {
                materializeMeasurement(j, member);
                ++_nSampledSoFar;
                _worksSinceLastAdvanced = 0;
                *out = id;
            } else {
                ++_specificStats.dupsDropped;
                ++_worksSinceLastAdvanced;
                _ws.free(id);
                return PlanStage::NEED_TIME;
            }
        } else {
            ++_specificStats.nBucketsDiscarded;
            ++_worksSinceLastAdvanced;
            _ws.free(id);
            return PlanStage::NEED_TIME;
        }
        uassert(5521504,
                str::stream() << kStageType << " could not find a non-duplicate measurement after "
                              << _worksSinceLastAdvanced << " attempts",
                _worksSinceLastAdvanced < _maxConsecutiveAttempts);
    } else if (PlanStage::NEED_YIELD == status) {
        *out = id;
    }
    return status;
}
}  // namespace mongo
