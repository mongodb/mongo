/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/s/analyze_shard_key_cmd_gen.h"
#include "mongo/s/analyze_shard_key_common_gen.h"
#include "mongo/s/analyze_shard_key_documents_gen.h"
#include "mongo/s/collection_routing_info_targeter.h"

namespace mongo {
namespace analyze_shard_key {

/**
 * The utility class for calculating read or write distribution metrics for sampled queries against
 * the collection with the given routing info.
 */
template <typename DistributionMetricsType, typename SampleSizeType>
class DistributionMetricsCalculator {
public:
    DistributionMetricsCalculator(const CollectionRoutingInfoTargeter& targeter)
        : _targeter(targeter),
          _firstShardKeyFieldName(
              _getChunkManager().getShardKeyPattern().toBSON().firstElement().fieldName()) {
        _getChunkManager().forEachChunk([&](const auto& chunk) {
            _numDispatchedByRange.emplace(std::make_pair(chunk.getRange(), 0));
            return true;
        });
    };

    /**
     * Calculates metrics for the given sampled query.
     */
    virtual void addQuery(OperationContext* opCtx, const SampledQueryDocument& doc) = 0;

    /**
     * Returns the metrics calculated based on the sampled queries added so far.
     */
    virtual DistributionMetricsType getMetrics() const = 0;

protected:
    virtual SampleSizeType _getSampleSize() const = 0;

    DistributionMetricsType _getMetrics() const;

    void _incrementTargetedOneShard() {
        _numTargetedOneShard++;
    }

    void _incrementTargetedMultipleShards() {
        _numTargetedMultipleShards++;
    }

    void _incrementTargetedAllShards() {
        _numTargetedAllShards++;
    }

    void _incrementTargetedRanges(const std::set<ChunkRange>& chunkRanges) {
        for (const auto& chunkRange : chunkRanges) {
            auto it = _numDispatchedByRange.find(chunkRange);
            invariant(it != _numDispatchedByRange.end());
            it->second++;
        }
    }

    /**
     * The helper for 'addQuery'. Increments the metrics for the query with the given filter(s),
     * collation, run-time contants and let parameters. The secondary filter is only applicable to
     * non-upsert replacement updates, and the run-time constants and let parameters are only
     * applicable to writes.
     *
     * If the query filters by shard key equality, returns the shard key value.
     */
    BSONObj _incrementMetricsForQuery(
        OperationContext* opCtx,
        const BSONObj& primaryfilter,
        const BSONObj& collation,
        const BSONObj& secondaryFilter = BSONObj(),
        const boost::optional<LegacyRuntimeConstants>& runtimeConstants = boost::none,
        const boost::optional<BSONObj>& letParameters = boost::none);

    const ChunkManager& _getChunkManager() const {
        return _targeter.getRoutingInfo().cm;
    }

    const ShardKeyPattern& _getShardKeyPattern() const {
        return _getChunkManager().getShardKeyPattern();
    }

    const CollatorInterface* _getDefaultCollator() const {
        return _getChunkManager().getDefaultCollator();
    }

    const CollectionRoutingInfoTargeter& _targeter;
    const StringData _firstShardKeyFieldName;

    int64_t _numTargetedOneShard = 0;
    int64_t _numTargetedMultipleShards = 0;
    int64_t _numTargetedAllShards = 0;

    std::map<ChunkRange, int64_t> _numDispatchedByRange;
};

class ReadDistributionMetricsCalculator
    : public DistributionMetricsCalculator<ReadDistributionMetrics, ReadSampleSize> {
public:
    ReadDistributionMetricsCalculator(const CollectionRoutingInfoTargeter& targeter)
        : DistributionMetricsCalculator(targeter) {}

    void addQuery(OperationContext* opCtx, const SampledQueryDocument& doc) override;

    ReadDistributionMetrics getMetrics() const override;

private:
    ReadSampleSize _getSampleSize() const override;

    int64_t _numFind = 0;
    int64_t _numAggregate = 0;
    int64_t _numCount = 0;
    int64_t _numDistinct = 0;
};

class WriteDistributionMetricsCalculator
    : public DistributionMetricsCalculator<WriteDistributionMetrics, WriteSampleSize> {
public:
    WriteDistributionMetricsCalculator(const CollectionRoutingInfoTargeter& targeter)
        : DistributionMetricsCalculator(targeter) {}

    void addQuery(OperationContext* opCtx, const SampledQueryDocument& doc) override;

    WriteDistributionMetrics getMetrics() const override;

private:
    WriteSampleSize _getSampleSize() const override;

    void _addUpdateQuery(OperationContext* opCtx, const write_ops::UpdateCommandRequest& cmd);

    void _addDeleteQuery(OperationContext* opCtx, const write_ops::DeleteCommandRequest& cmd);

    void _addFindAndModifyQuery(OperationContext* opCtx,
                                const write_ops::FindAndModifyCommandRequest& cmd);

    void _incrementShardKeyUpdates() {
        _numShardKeyUpdates++;
    }

    void _incrementSingleWritesWithoutShardKey() {
        _numSingleWritesWithoutShardKey++;
    }

    void _incrementMultiWritesWithoutShardKey() {
        _numMultiWritesWithoutShardKey++;
    }

    void _incrementMetricsForQuery(OperationContext* opCtx,
                                   const BSONObj& primaryFilter,
                                   const BSONObj& secondaryFilter,
                                   const BSONObj& collation,
                                   bool isMulti,
                                   const boost::optional<LegacyRuntimeConstants>& runtimeConstants,
                                   const boost::optional<BSONObj>& letParameters);

    int64_t _numUpdate = 0;
    int64_t _numDelete = 0;
    int64_t _numFindAndModify = 0;

    int64_t _numShardKeyUpdates = 0;
    int64_t _numSingleWritesWithoutShardKey = 0;
    int64_t _numMultiWritesWithoutShardKey = 0;
};

}  // namespace analyze_shard_key
}  // namespace mongo
