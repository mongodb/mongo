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

#pragma once

#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/exec/shard_filterer.h"
#include "mongo/db/exec/timeseries/bucket_unpacker.h"

namespace mongo {
/**
 * This stage implements a variation on the ARHASH algorithm (see
 * https://dl.acm.org/doi/10.1145/93605.98746), by running one iteration of the ARHASH algorithm to
 * materialze a random measurement from a randomly sampled bucket once per doWork() call. The plan
 * is constructed such that the input documents to this stage are coming from a storage-provided
 * random cursor.
 */
class SampleFromTimeseriesBucket final : public PlanStage {
public:
    static const char* kStageType;

    /**
     * Constructs a 'SampleFromTimeseriesBucket' stage which uses 'bucketUnpacker' to materialize
     * the sampled measurment from the buckets returned by the child stage.
     *  - 'sampleSize' is the user-requested number of documents to sample.
     *  - 'maxConsecutiveAttempts' configures the maximum number of consecutive "misses" when
     *  performing the ARHASH algorithm. A miss may happen either when we sample a duplicate, or the
     *  index 'j' selected by the PRNG exceeds the number of measurements in the bucket. If we miss
     *  enough times in a row, we throw an exception that terminates the execution of the query.
     *  - 'bucketMaxCount' is the maximum number of measurements allowed in a bucket, which can be
     *  configured via a server parameter.
     */
    SampleFromTimeseriesBucket(ExpressionContext* expCtx,
                               WorkingSet* ws,
                               std::unique_ptr<PlanStage> child,
                               BucketUnpacker bucketUnpacker,
                               boost::optional<std::unique_ptr<ShardFilterer>> shardFilterer,
                               int maxConsecutiveAttempts,
                               long long sampleSize,
                               int bucketMaxCount);

    StageType stageType() const final {
        return STAGE_SAMPLE_FROM_TIMESERIES_BUCKET;
    }

    bool isEOF() final {
        return _nSampledSoFar >= _sampleSize;
    }

    std::unique_ptr<PlanStageStats> getStats() final;

    const SpecificStats* getSpecificStats() const final {
        return &_specificStats;
    }

    PlanStage::StageState doWork(WorkingSetID* id);

private:
    /**
     * Carries the bucket _id and index for the measurement that was sampled.
     */
    struct SampledMeasurementKey {
        SampledMeasurementKey(OID bucketId, int64_t measurementIndex)
            : bucketId(bucketId), measurementIndex(measurementIndex) {}

        bool operator==(const SampledMeasurementKey& key) const {
            return this->bucketId == key.bucketId && this->measurementIndex == key.measurementIndex;
        }

        OID bucketId;
        int32_t measurementIndex;
    };

    /**
     * Computes a hash of 'SampledMeasurementKey' so measurements that have already been seen can
     * be kept track of for de-duplication after sampling.
     */
    struct SampledMeasurementKeyHasher {
        size_t operator()(const SampledMeasurementKey& s) const {
            return absl::Hash<uint64_t>{}(s.bucketId.view().read<uint64_t>()) ^
                absl::Hash<uint32_t>{}(s.bucketId.view().read<uint32_t>(8)) ^
                absl::Hash<int32_t>{}(s.measurementIndex);
        }
    };

    // Tracks which measurements have been seen so far.
    using SeenSet = stdx::unordered_set<SampledMeasurementKey, SampledMeasurementKeyHasher>;

    void materializeMeasurement(int64_t measurementIdx, WorkingSetMember* out);

    WorkingSet& _ws;
    BucketUnpacker _bucketUnpacker;
    boost::optional<std::unique_ptr<ShardFilterer>> _shardFilterer;
    SampleFromTimeseriesBucketStats _specificStats;

    const int _maxConsecutiveAttempts;
    const long long _sampleSize;
    const int _bucketMaxCount;

    int _worksSinceLastAdvanced = 0;
    long long _nSampledSoFar = 0;

    // Used to de-duplicate randomly sampled measurements.
    SeenSet _seenSet;
};
}  //  namespace mongo
