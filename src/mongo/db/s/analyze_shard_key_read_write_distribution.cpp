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

#include "mongo/platform/basic.h"

#include "mongo/db/s/analyze_shard_key_read_write_distribution.h"

#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/query/collation/collation_index_key.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/s/shard_key_index_util.h"
#include "mongo/logv2/log.h"
#include "mongo/s/analyze_shard_key_util.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/grid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace analyze_shard_key {

namespace {

/**
 * Returns true if the query that specifies the given collation against the collection with the
 * given default collator has simple collation.
 */
bool hasSimpleCollation(const CollatorInterface* defaultCollator, const BSONObj& collation) {
    if (collation.isEmpty()) {
        return !defaultCollator;
    }
    return SimpleBSONObjComparator::kInstance.evaluate(collation == CollationSpec::kSimpleSpec);
}

/**
 * Returns true if the given shard key contains any collatable fields (ones that can be affected in
 * comparison or sort order by collation).
 */
bool shardKeyHasCollatableType(const ShardKeyPattern& shardKeyPattern, const BSONObj& shardKey) {
    for (const BSONElement& elt : shardKey) {
        if (CollationIndexKey::isCollatableType(elt.type())) {
            return true;
        }
        if (shardKeyPattern.isHashedPattern() &&
            shardKeyPattern.getHashedField().fieldNameStringData() == elt.fieldNameStringData()) {
            // If the field is specified as "hashed" in the shard key pattern, then the hash value
            // could have come from a collatable type.
            return true;
        }
    }
    return false;
}

}  // namespace

template <typename DistributionMetricsType, typename SampleSizeType>
DistributionMetricsType
DistributionMetricsCalculator<DistributionMetricsType, SampleSizeType>::_getMetrics() const {
    DistributionMetricsType metrics(_getSampleSize());
    if (auto numTotal = metrics.getSampleSize().getTotal(); numTotal > 0) {
        metrics.setNumTargetedOneShard(_numTargetedOneShard);
        metrics.setPercentageOfTargetedOneShard(
            calculatePercentage(_numTargetedOneShard, numTotal));

        metrics.setNumTargetedMultipleShards(_numTargetedMultipleShards);
        metrics.setPercentageOfTargetedMultipleShards(
            calculatePercentage(_numTargetedMultipleShards, numTotal));

        metrics.setNumTargetedAllShards(_numTargetedAllShards);
        metrics.setPercentageOfTargetedAllShards(
            calculatePercentage(_numTargetedAllShards, numTotal));

        std::vector<int64_t> numDispatchedByRange;
        for (auto& [_, numDispatched] : _numDispatchedByRange) {
            numDispatchedByRange.push_back(numDispatched);
        }
        metrics.setNumDispatchedByRange(numDispatchedByRange);
    }
    return metrics;
}

template <typename DistributionMetricsType, typename SampleSizeType>
BSONObj
DistributionMetricsCalculator<DistributionMetricsType, SampleSizeType>::_incrementMetricsForQuery(
    OperationContext* opCtx,
    const BSONObj& primaryFilter,
    const BSONObj& collation,
    const BSONObj& secondaryFilter,
    const boost::optional<LegacyRuntimeConstants>& runtimeConstants,
    const boost::optional<BSONObj>& letParameters) {
    auto filter = primaryFilter;
    auto shardKey = uassertStatusOK(
        _getShardKeyPattern().extractShardKeyFromQuery(opCtx, _targeter.getNS(), primaryFilter));
    if (shardKey.isEmpty() && !secondaryFilter.isEmpty()) {
        filter = secondaryFilter;
        shardKey = _getShardKeyPattern().extractShardKeyFromDoc(secondaryFilter);
    }

    // Increment metrics about range targeting.
    auto&& cif = [&]() {
        if (collation.isEmpty()) {
            return std::unique_ptr<CollatorInterface>{};
        }
        return uassertStatusOK(
            CollatorFactoryInterface::get(opCtx->getServiceContext())->makeFromBSON(collation));
    }();
    auto expCtx = make_intrusive<ExpressionContext>(
        opCtx, std::move(cif), _getChunkManager().getNss(), runtimeConstants, letParameters);

    std::set<ShardId> shardIds;  // This is not used.
    std::set<ChunkRange> chunkRanges;
    bool targetMinkeyToMaxKey = false;
    _getChunkManager().getShardIdsForQuery(
        expCtx, filter, collation, &shardIds, &chunkRanges, &targetMinkeyToMaxKey);
    _incrementTargetedRanges(chunkRanges);

    // Increment metrics about sharding targeting.
    if (!shardKey.isEmpty()) {
        // This query filters by shard key equality. If the query has a simple collation or the
        // shard key doesn't contain a collatable field, then there is only one matching shard key
        // value so the query is guaranteed to target only one shard. Otherwise, the number of
        // shards that it targets depend on how the matching shard key values are distributed among
        // shards. Given this, pessimistically classify it as targeting to multiple shards.
        invariant(!targetMinkeyToMaxKey);
        if (hasSimpleCollation(_getDefaultCollator(), collation) ||
            !shardKeyHasCollatableType(_getShardKeyPattern(), shardKey)) {
            _incrementTargetedOneShard();
            invariant(chunkRanges.size() == 1U);
        } else {
            _incrementTargetedMultipleShards();
        }
    } else if (targetMinkeyToMaxKey) {
        // This query targets the entire shard key space. Therefore, it always targets all
        // shards and chunks.
        _incrementTargetedAllShards();
        invariant((int)chunkRanges.size() == _getChunkManager().numChunks());
    } else {
        // This query targets a subset of the shard key space. Therefore, the number of shards
        // that it targets depends on how the matching shard key ranges are distributed among
        // shards. Given this, pessimistically classify it as targeting to multiple shards.
        _incrementTargetedMultipleShards();
    }

    return shardKey;
}

ReadSampleSize ReadDistributionMetricsCalculator::_getSampleSize() const {
    ReadSampleSize sampleSize;
    sampleSize.setTotal(_numFind + _numAggregate + _numCount + _numDistinct);
    sampleSize.setFind(_numFind);
    sampleSize.setAggregate(_numAggregate);
    sampleSize.setCount(_numCount);
    sampleSize.setDistinct(_numDistinct);
    return sampleSize;
}

ReadDistributionMetrics ReadDistributionMetricsCalculator::getMetrics() const {
    return _getMetrics();
}

void ReadDistributionMetricsCalculator::addQuery(OperationContext* opCtx,
                                                 const SampledQueryDocument& doc) {
    switch (doc.getCmdName()) {
        case SampledCommandNameEnum::kFind:
            _numFind++;
            break;
        case SampledCommandNameEnum::kAggregate:
            _numAggregate++;
            break;
        case SampledCommandNameEnum::kCount:
            _numCount++;
            break;
        case SampledCommandNameEnum::kDistinct:
            _numDistinct++;
            break;
        default:
            MONGO_UNREACHABLE;
    }

    auto cmd = SampledReadCommand::parse(IDLParserContext("ReadDistributionMetricsCalculator"),
                                         doc.getCmd());
    _incrementMetricsForQuery(opCtx, cmd.getFilter(), cmd.getCollation());
}

WriteSampleSize WriteDistributionMetricsCalculator::_getSampleSize() const {
    WriteSampleSize sampleSize;
    sampleSize.setTotal(_numUpdate + _numDelete + _numFindAndModify);
    sampleSize.setUpdate(_numUpdate);
    sampleSize.setDelete(_numDelete);
    sampleSize.setFindAndModify(_numFindAndModify);
    return sampleSize;
}

WriteDistributionMetrics WriteDistributionMetricsCalculator::getMetrics() const {
    auto metrics = DistributionMetricsCalculator::_getMetrics();
    if (auto numTotal = metrics.getSampleSize().getTotal(); numTotal > 0) {
        metrics.setNumShardKeyUpdates(_numShardKeyUpdates);
        metrics.setPercentageOfShardKeyUpdates(calculatePercentage(_numShardKeyUpdates, numTotal));

        metrics.setNumSingleWritesWithoutShardKey(_numSingleWritesWithoutShardKey);
        metrics.setPercentageOfSingleWritesWithoutShardKey(
            calculatePercentage(_numSingleWritesWithoutShardKey, numTotal));

        metrics.setNumMultiWritesWithoutShardKey(_numMultiWritesWithoutShardKey);
        metrics.setPercentageOfMultiWritesWithoutShardKey(
            calculatePercentage(_numMultiWritesWithoutShardKey, numTotal));
    }
    return metrics;
}

void WriteDistributionMetricsCalculator::addQuery(OperationContext* opCtx,
                                                  const SampledQueryDocument& doc) {
    switch (doc.getCmdName()) {
        case SampledCommandNameEnum::kUpdate: {
            auto cmd = write_ops::UpdateCommandRequest::parse(
                IDLParserContext("WriteDistributionMetricsCalculator"), doc.getCmd());
            _addUpdateQuery(opCtx, cmd);
            break;
        }
        case SampledCommandNameEnum::kDelete: {
            auto cmd = write_ops::DeleteCommandRequest::parse(
                IDLParserContext("WriteDistributionMetricsCalculator"), doc.getCmd());
            _addDeleteQuery(opCtx, cmd);
            break;
        }
        case SampledCommandNameEnum::kFindAndModify: {
            auto cmd = write_ops::FindAndModifyCommandRequest::parse(
                IDLParserContext("WriteDistributionMetricsCalculator"), doc.getCmd());
            _addFindAndModifyQuery(opCtx, cmd);
            break;
        }
        default:
            MONGO_UNREACHABLE;
    }
}

void WriteDistributionMetricsCalculator::_addUpdateQuery(
    OperationContext* opCtx, const write_ops::UpdateCommandRequest& cmd) {
    for (const auto& updateOp : cmd.getUpdates()) {
        _numUpdate++;
        auto primaryFilter = updateOp.getQ();
        // If this is a non-upsert replacement update, the replacement document can be used as a
        // filter.
        auto secondaryFilter = !updateOp.getUpsert() &&
                updateOp.getU().type() == write_ops::UpdateModification::Type::kReplacement
            ? updateOp.getU().getUpdateReplacement()
            : BSONObj();
        _incrementMetricsForQuery(opCtx,
                                  primaryFilter,
                                  secondaryFilter,
                                  write_ops::collationOf(updateOp),
                                  updateOp.getMulti(),
                                  cmd.getLegacyRuntimeConstants(),
                                  cmd.getLet());
    }
}

void WriteDistributionMetricsCalculator::_addDeleteQuery(
    OperationContext* opCtx, const write_ops::DeleteCommandRequest& cmd) {
    for (const auto& deleteOp : cmd.getDeletes()) {
        _numDelete++;
        auto primaryFilter = deleteOp.getQ();
        auto secondaryFilter = BSONObj();
        _incrementMetricsForQuery(opCtx,
                                  primaryFilter,
                                  secondaryFilter,
                                  write_ops::collationOf(deleteOp),
                                  deleteOp.getMulti(),
                                  cmd.getLegacyRuntimeConstants(),
                                  cmd.getLet());
    }
}

void WriteDistributionMetricsCalculator::_addFindAndModifyQuery(
    OperationContext* opCtx, const write_ops::FindAndModifyCommandRequest& cmd) {
    _numFindAndModify++;
    auto primaryFilter = cmd.getQuery();
    auto secondaryFilter = BSONObj();
    _incrementMetricsForQuery(opCtx,
                              primaryFilter,
                              secondaryFilter,
                              cmd.getCollation().value_or(BSONObj()),
                              false /* isMulti */,
                              cmd.getLegacyRuntimeConstants(),
                              cmd.getLet());
}

void WriteDistributionMetricsCalculator::_incrementMetricsForQuery(
    OperationContext* opCtx,
    const BSONObj& primaryFilter,
    const BSONObj& secondaryFilter,
    const BSONObj& collation,
    bool isMulti,
    const boost::optional<LegacyRuntimeConstants>& runtimeConstants,
    const boost::optional<BSONObj>& letParameters) {
    auto shardKey = DistributionMetricsCalculator::_incrementMetricsForQuery(
        opCtx, primaryFilter, collation, secondaryFilter, runtimeConstants, letParameters);

    if (shardKey.isEmpty()) {
        // Increment metrics about writes without shard key.
        if (isMulti) {
            _incrementMultiWritesWithoutShardKey();
        } else {
            _incrementSingleWritesWithoutShardKey();
        }
    }
}

}  // namespace analyze_shard_key
}  // namespace mongo
