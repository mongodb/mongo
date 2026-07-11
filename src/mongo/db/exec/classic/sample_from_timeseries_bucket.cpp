// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


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

#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


namespace mongo {

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
