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
#include "mongo/s/analyze_shard_key_server_parameters_gen.h"
#include "mongo/s/analyze_shard_key_util.h"
#include "mongo/s/collection_routing_info_targeter.h"
#include "mongo/s/shard_key_pattern_query_util.h"

namespace mongo {
namespace analyze_shard_key {

namespace {
using QueryTargetingInfo = shard_key_pattern_query_util::QueryTargetingInfo;
}

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
            _numByRange.emplace(std::make_pair(chunk.getRange(), 0));
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

    /**
     * Returns the total number of sampled queries added so far.
     */
    virtual int64_t getNumTotal() const = 0;

protected:
    virtual SampleSizeType _getSampleSize() const = 0;

    DistributionMetricsType _getMetrics() const;

    void _incrementNumSingleShard() {
        _numSingleShard++;
    }

    void _incrementNumMultiShard() {
        _numMultiShard++;
    }

    void _incrementNumScatterGather() {
        _numScatterGather++;
    }

    void _incrementNumByRanges(const std::set<ChunkRange>& chunkRanges) {
        for (const auto& chunkRange : chunkRanges) {
            auto it = _numByRange.find(chunkRange);
            invariant(it != _numByRange.end());
            it->second++;
        }
    }

    /**
     * The helper used by 'addQuery' to get the targeting info for a query with the given filter,
     * collation, let parameters and runtime contants.
     */
    QueryTargetingInfo _getTargetingInfoForQuery(
        OperationContext* opCtx,
        const BSONObj& filter,
        const BSONObj& collation,
        const boost::optional<BSONObj>& letParameters = boost::none,
        const boost::optional<LegacyRuntimeConstants>& runtimeConstants = boost::none);

    /**
     * The helper used by 'addQuery' to increment the metrics for a query with the given targeting
     * info.
     */
    void _incrementMetricsForQuery(const QueryTargetingInfo& info);

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

    int64_t _numSingleShard = 0;
    int64_t _numMultiShard = 0;
    int64_t _numScatterGather = 0;

    std::map<ChunkRange, int64_t> _numByRange;
};

class ReadDistributionMetricsCalculator
    : public DistributionMetricsCalculator<ReadDistributionMetrics, ReadSampleSize> {
public:
    ReadDistributionMetricsCalculator(const CollectionRoutingInfoTargeter& targeter)
        : DistributionMetricsCalculator(targeter) {}

    void addQuery(OperationContext* opCtx, const SampledQueryDocument& doc) override;

    ReadDistributionMetrics getMetrics() const override;

    int64_t getNumTotal() const override {
        return _numFind + _numAggregate + _numCount + _numDistinct;
    }

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

    int64_t getNumTotal() const override {
        return _numUpdate + _numDelete + _numFindAndModify;
    }

    int64_t getNumShardKeyUpdates() const {
        return _numShardKeyUpdates;
    }

    void setNumShardKeyUpdates(int64_t num) {
        invariant(num >= 0);
        _numShardKeyUpdates = num;
    }

private:
    WriteSampleSize _getSampleSize() const override;

    void _addUpdateQuery(OperationContext* opCtx, const write_ops::UpdateCommandRequest& cmd);

    void _addDeleteQuery(OperationContext* opCtx, const write_ops::DeleteCommandRequest& cmd);

    void _addFindAndModifyQuery(OperationContext* opCtx,
                                const write_ops::FindAndModifyCommandRequest& cmd);

    void _incrementNumSingleWritesWithoutShardKey() {
        _numSingleWritesWithoutShardKey++;
    }

    void _incrementNumMultiWritesWithoutShardKey() {
        _numMultiWritesWithoutShardKey++;
    }

    void _incrementMetricsForQuery(const QueryTargetingInfo& info, bool isMulti);

    int64_t _numUpdate = 0;
    int64_t _numDelete = 0;
    int64_t _numFindAndModify = 0;

    int64_t _numShardKeyUpdates = 0;
    int64_t _numSingleWritesWithoutShardKey = 0;
    int64_t _numMultiWritesWithoutShardKey = 0;
};

// Override the + operator and == operator for ReadSampleSize, WriteSampleSize,
// ReadDistributionMetrics and WriteDistributionMetrics.

inline ReadSampleSize operator+(const ReadSampleSize& l, const ReadSampleSize& r) {
    ReadSampleSize sampleSize;
    sampleSize.setTotal(l.getTotal() + r.getTotal());
    sampleSize.setFind(l.getFind() + r.getFind());
    sampleSize.setAggregate(l.getAggregate() + r.getAggregate());
    sampleSize.setCount(l.getCount() + r.getCount());
    sampleSize.setDistinct(l.getDistinct() + r.getDistinct());
    return sampleSize;
}

inline bool operator==(const ReadSampleSize& l, const ReadSampleSize& r) {
    return l.getTotal() == r.getTotal() && l.getFind() == r.getFind() &&
        l.getAggregate() == r.getAggregate() && l.getCount() == r.getCount() &&
        l.getDistinct() == r.getDistinct();
}

inline WriteSampleSize operator+(const WriteSampleSize& l, const WriteSampleSize& r) {
    WriteSampleSize sampleSize;
    sampleSize.setTotal(l.getTotal() + r.getTotal());
    sampleSize.setUpdate(l.getUpdate() + r.getUpdate());
    sampleSize.setDelete(l.getDelete() + r.getDelete());
    sampleSize.setFindAndModify(l.getFindAndModify() + r.getFindAndModify());
    return sampleSize;
}

inline bool operator==(const WriteSampleSize& l, const WriteSampleSize& r) {
    return l.getTotal() == r.getTotal() && l.getUpdate() == r.getUpdate() &&
        l.getDelete() == r.getDelete() && l.getFindAndModify() == r.getFindAndModify();
}

template <typename T>
std::vector<T> addNumByRange(const std::vector<T>& l, const std::vector<T>& r) {
    invariant(!l.empty());
    invariant(!r.empty());
    uassert(
        7559401,
        str::stream()
            << "Failed to combine the 'numByRange' metrics from two shards since one has length "
            << l.size() << " and the other one has length " << r.size()
            << ". This is likely because one of the shard fetched the split point documents after "
               "TTL deletions had started. The lifetime of the split point documents is "
               "configurable via 'analyzeShardKeySplitPointExpirationSecs' which is currently set "
               "to "
            << gAnalyzeShardKeySplitPointExpirationSecs.load()
            << ". Please retry the command again.",
        l.size() == r.size());

    std::vector<T> result;
    result.reserve(l.size());

    std::transform(l.begin(), l.end(), r.begin(), std::back_inserter(result), std::plus<T>());
    return result;
}

template <typename DistributionMetricsType>
DistributionMetricsType addDistributionMetricsBase(DistributionMetricsType l,
                                                   DistributionMetricsType r) {
    DistributionMetricsType metrics;
    metrics.setSampleSize(l.getSampleSize() + r.getSampleSize());
    if (auto numTotal = metrics.getSampleSize().getTotal(); numTotal > 0) {
        auto numSingleShard = l.getNumSingleShard().value_or(0) + r.getNumSingleShard().value_or(0);
        metrics.setNumSingleShard(numSingleShard);
        metrics.setPercentageOfSingleShard(calculatePercentage(numSingleShard, numTotal));

        auto numMultiShard = l.getNumMultiShard().value_or(0) + r.getNumMultiShard().value_or(0);
        metrics.setNumMultiShard(numMultiShard);
        metrics.setPercentageOfMultiShard(calculatePercentage(numMultiShard, numTotal));

        auto numScatterGather =
            l.getNumScatterGather().value_or(0) + r.getNumScatterGather().value_or(0);
        metrics.setNumScatterGather(numScatterGather);
        metrics.setPercentageOfScatterGather(calculatePercentage(numScatterGather, numTotal));

        if (l.getNumByRange() && r.getNumByRange()) {
            metrics.setNumByRange(addNumByRange(*l.getNumByRange(), *r.getNumByRange()));
        } else if (l.getNumByRange()) {
            metrics.setNumByRange(*l.getNumByRange());
        } else if (r.getNumByRange()) {
            metrics.setNumByRange(*r.getNumByRange());
        }
    }
    return metrics;
}

template <typename DistributionMetricsType>
inline bool areEqualDistributionMetricsBase(const DistributionMetricsType& l,
                                            const DistributionMetricsType& r) {
    return l.getSampleSize() == r.getSampleSize() &&
        l.getNumSingleShard() == r.getNumSingleShard() &&
        l.getPercentageOfSingleShard() == r.getPercentageOfSingleShard() &&
        l.getNumMultiShard() == r.getNumMultiShard() &&
        l.getPercentageOfMultiShard() == r.getPercentageOfMultiShard() &&
        l.getNumScatterGather() == r.getNumScatterGather() &&
        l.getPercentageOfScatterGather() == r.getPercentageOfScatterGather();
}

inline ReadDistributionMetrics operator+(const ReadDistributionMetrics& l,
                                         const ReadDistributionMetrics& r) {
    auto metrics = addDistributionMetricsBase(l, r);
    return metrics;
}

inline bool operator==(const ReadDistributionMetrics& l, const ReadDistributionMetrics& r) {
    return areEqualDistributionMetricsBase(l, r);
}

inline bool operator!=(const ReadDistributionMetrics& l, const ReadDistributionMetrics& r) {
    return !(l == r);
}

inline WriteDistributionMetrics operator+(const WriteDistributionMetrics& l,
                                          const WriteDistributionMetrics& r) {
    auto metrics = addDistributionMetricsBase(l, r);
    if (auto numTotal = metrics.getSampleSize().getTotal(); numTotal > 0) {
        auto numShardKeyUpdates =
            l.getNumShardKeyUpdates().value_or(0) + r.getNumShardKeyUpdates().value_or(0);
        metrics.setNumShardKeyUpdates(numShardKeyUpdates);
        metrics.setPercentageOfShardKeyUpdates(calculatePercentage(numShardKeyUpdates, numTotal));

        auto numSingleWritesWithoutShardKey = l.getNumSingleWritesWithoutShardKey().value_or(0) +
            r.getNumSingleWritesWithoutShardKey().value_or(0);
        metrics.setNumSingleWritesWithoutShardKey(numSingleWritesWithoutShardKey);
        metrics.setPercentageOfSingleWritesWithoutShardKey(
            calculatePercentage(numSingleWritesWithoutShardKey, numTotal));

        auto numMultiWritesWithoutShardKey = l.getNumMultiWritesWithoutShardKey().value_or(0) +
            r.getNumMultiWritesWithoutShardKey().value_or(0);
        metrics.setNumMultiWritesWithoutShardKey(numMultiWritesWithoutShardKey);
        metrics.setPercentageOfMultiWritesWithoutShardKey(
            calculatePercentage(numMultiWritesWithoutShardKey, numTotal));
    }
    return metrics;
}

inline bool operator==(const WriteDistributionMetrics& l, const WriteDistributionMetrics& r) {
    return areEqualDistributionMetricsBase(l, r) &&
        l.getNumShardKeyUpdates() == r.getNumShardKeyUpdates() &&
        l.getPercentageOfShardKeyUpdates() == r.getPercentageOfShardKeyUpdates() &&
        l.getNumSingleWritesWithoutShardKey() == r.getNumSingleWritesWithoutShardKey() &&
        l.getPercentageOfSingleWritesWithoutShardKey() ==
        r.getPercentageOfSingleWritesWithoutShardKey() &&
        l.getNumMultiWritesWithoutShardKey() == r.getNumMultiWritesWithoutShardKey() &&
        l.getPercentageOfMultiWritesWithoutShardKey() ==
        r.getPercentageOfMultiWritesWithoutShardKey();
}

inline bool operator!=(const WriteDistributionMetrics& l, const WriteDistributionMetrics& r) {
    return !(l == r);
}

}  // namespace analyze_shard_key
}  // namespace mongo
