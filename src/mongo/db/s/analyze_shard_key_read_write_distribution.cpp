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

#include "mongo/db/s/analyze_shard_key_read_write_distribution.h"

#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/internal_transactions_feature_flag_gen.h"
#include "mongo/db/query/collation/collation_index_key.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/s/shard_key_index_util.h"
#include "mongo/logv2/log.h"
#include "mongo/s/analyze_shard_key_util.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/collection_routing_info_targeter.h"
#include "mongo/s/grid.h"
#include "mongo/s/shard_key_pattern_query_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace analyze_shard_key {

template <typename DistributionMetricsType, typename SampleSizeType>
DistributionMetricsType
DistributionMetricsCalculator<DistributionMetricsType, SampleSizeType>::_getMetrics() const {
    DistributionMetricsType metrics(_getSampleSize());
    if (auto numTotal = metrics.getSampleSize().getTotal(); numTotal > 0) {
        metrics.setNumSingleShard(_numSingleShard);
        metrics.setPercentageOfSingleShard(calculatePercentage(_numSingleShard, numTotal));

        metrics.setNumMultiShard(_numMultiShard);
        metrics.setPercentageOfMultiShard(calculatePercentage(_numMultiShard, numTotal));

        metrics.setNumScatterGather(_numScatterGather);
        metrics.setPercentageOfScatterGather(calculatePercentage(_numScatterGather, numTotal));

        std::vector<int64_t> numByRange;
        for (auto& [_, num] : _numByRange) {
            numByRange.push_back(num);
        }
        metrics.setNumByRange(numByRange);
    }
    return metrics;
}

template <typename DistributionMetricsType, typename SampleSizeType>
QueryTargetingInfo
DistributionMetricsCalculator<DistributionMetricsType, SampleSizeType>::_getTargetingInfoForQuery(
    OperationContext* opCtx,
    const BSONObj& filter,
    const BSONObj& collation,
    const boost::optional<BSONObj>& letParameters,
    const boost::optional<LegacyRuntimeConstants>& runtimeConstants) {
    auto&& cif = [&]() {
        if (collation.isEmpty()) {
            return std::unique_ptr<CollatorInterface>{};
        }
        return uassertStatusOK(
            CollatorFactoryInterface::get(opCtx->getServiceContext())->makeFromBSON(collation));
    }();
    auto expCtx = make_intrusive<ExpressionContext>(
        opCtx,
        std::move(cif),
        _getChunkManager().getNss(),
        runtimeConstants.value_or(Variables::generateRuntimeConstants(opCtx)),
        letParameters);

    std::set<ShardId> shardIds;  // This is not used.
    QueryTargetingInfo info;
    getShardIdsForQuery(expCtx, filter, collation, _getChunkManager(), &shardIds, &info);

    return info;
}

template <typename DistributionMetricsType, typename SampleSizeType>
void DistributionMetricsCalculator<DistributionMetricsType, SampleSizeType>::
    _incrementMetricsForQuery(const QueryTargetingInfo& info) {
    // Increment metrics about range targeting.
    _incrementNumByRanges(info.chunkRanges);

    // Increment metrics about shard targeting.
    switch (info.desc) {
        case QueryTargetingInfo::Description::kSingleKey: {
            _incrementNumSingleShard();
            tassert(7531200,
                    "Found a point query that targets multiple chunks",
                    info.chunkRanges.size() == 1U);
            break;
        }
        case QueryTargetingInfo::Description::kMultipleKeys: {
            // This query targets a subset of the shard key space. Therefore, the number of shards
            // that it targets depends on how the matching shard key ranges are distributed among
            // shards. Given this, pessimistically classify it as targeting to multiple shards.
            _incrementNumMultiShard();
            break;
        }
        case QueryTargetingInfo::Description::kMinKeyToMaxKey: {
            // This query targets the entire shard key space. Therefore, it always targets all
            // shards and chunks.
            _incrementNumScatterGather();
            invariant((int)info.chunkRanges.size() == _getChunkManager().numChunks());
            break;
        }
        default:
            MONGO_UNREACHABLE;
    }
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
    auto info = _getTargetingInfoForQuery(opCtx, cmd.getFilter(), cmd.getCollation(), cmd.getLet());
    _incrementMetricsForQuery(info);
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
        auto collation = write_ops::collationOf(updateOp);
        auto info = _getTargetingInfoForQuery(
            opCtx, updateOp.getQ(), collation, cmd.getLet(), cmd.getLegacyRuntimeConstants());

        if (info.desc != QueryTargetingInfo::Description::kSingleKey) {
            // If this is a non-upsert replacement update, the replacement document can be used as
            // the filter.
            auto isReplacementUpdate = !updateOp.getUpsert() &&
                updateOp.getU().type() == write_ops::UpdateModification::Type::kReplacement;
            auto isExactIdQuery = [&] {
                return CollectionRoutingInfoTargeter::isExactIdQuery(
                    opCtx, cmd.getNamespace(), updateOp.getQ(), collation, _getChunkManager());
            };

            // Currently, targeting by replacement document is only done when an updateOne without
            // shard key is not supported or when the query targets an exact id value.
            if (isReplacementUpdate &&
                (!feature_flags::gFeatureFlagUpdateOneWithoutShardKey.isEnabled(
                     serverGlobalParams.featureCompatibility) ||
                 isExactIdQuery())) {
                auto filter = _getShardKeyPattern().extractShardKeyFromDoc(
                    updateOp.getU().getUpdateReplacement());
                info = _getTargetingInfoForQuery(
                    opCtx, filter, collation, cmd.getLet(), cmd.getLegacyRuntimeConstants());
            }
        }
        _incrementMetricsForQuery(info, updateOp.getMulti());
    }
}

void WriteDistributionMetricsCalculator::_addDeleteQuery(
    OperationContext* opCtx, const write_ops::DeleteCommandRequest& cmd) {
    for (const auto& deleteOp : cmd.getDeletes()) {
        _numDelete++;
        auto info = _getTargetingInfoForQuery(opCtx,
                                              deleteOp.getQ(),
                                              write_ops::collationOf(deleteOp),
                                              cmd.getLet(),
                                              cmd.getLegacyRuntimeConstants());
        _incrementMetricsForQuery(info, deleteOp.getMulti());
    }
}

void WriteDistributionMetricsCalculator::_addFindAndModifyQuery(
    OperationContext* opCtx, const write_ops::FindAndModifyCommandRequest& cmd) {
    _numFindAndModify++;
    auto info = _getTargetingInfoForQuery(opCtx,
                                          cmd.getQuery(),
                                          cmd.getCollation().value_or(BSONObj()),
                                          cmd.getLet(),
                                          cmd.getLegacyRuntimeConstants());
    _incrementMetricsForQuery(info, false /* isMulti */);
}

void WriteDistributionMetricsCalculator::_incrementMetricsForQuery(const QueryTargetingInfo& info,
                                                                   bool isMulti) {
    DistributionMetricsCalculator::_incrementMetricsForQuery(info);
    if (info.desc != QueryTargetingInfo::Description::kSingleKey) {
        // Increment metrics about writes without shard key.
        if (isMulti) {
            _incrementNumMultiWritesWithoutShardKey();
        } else {
            _incrementNumSingleWritesWithoutShardKey();
        }
    }
}

}  // namespace analyze_shard_key
}  // namespace mongo
