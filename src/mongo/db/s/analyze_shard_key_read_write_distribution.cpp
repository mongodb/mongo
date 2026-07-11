// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/s/analyze_shard_key_read_write_distribution.h"

#include "mongo/base/status_with.h"
#include "mongo/db/commands/query_cmd/bulk_write_crud_op.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/write_ops/write_ops.h"
#include "mongo/db/query/write_ops/write_ops_parsers.h"
#include "mongo/db/router_role/collection_routing_info_targeter.h"
#include "mongo/db/s/analyze_shard_key_util.h"
#include "mongo/db/server_options.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/s/analyze_shard_key_common_gen.h"
#include "mongo/s/query/exec/target_write_op.h"
#include "mongo/s/query/shard_key_pattern_query_util.h"
#include "mongo/util/intrusive_counter.h"

#include <memory>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

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
    const auto& cm = _getChunkManager();
    auto expCtx =
        ExpressionContextBuilder{}
            .opCtx(opCtx)
            .collator(std::move(cif))
            .ns(cm.getNss())
            .runtimeConstants(runtimeConstants.value_or(Variables::generateRuntimeConstants(opCtx)))
            .letParameters(letParameters)
            .build();

    std::set<ShardId> shardIds;  // This is not used.
    QueryTargetingInfo info;
    getShardIdsAndChunksForQuery(expCtx, filter, collation, cm, &shardIds, &info);

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

    auto cmd = SampledReadCommand::parse(doc.getCmd(),
                                         IDLParserContext("ReadDistributionMetricsCalculator"));
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
                doc.getCmd(), IDLParserContext("WriteDistributionMetricsCalculator"));
            _addUpdateQuery(opCtx, cmd);
            break;
        }
        case SampledCommandNameEnum::kDelete: {
            auto cmd = write_ops::DeleteCommandRequest::parse(
                doc.getCmd(), IDLParserContext("WriteDistributionMetricsCalculator"));
            _addDeleteQuery(opCtx, cmd);
            break;
        }
        case SampledCommandNameEnum::kFindAndModify: {
            auto cmd = write_ops::FindAndModifyCommandRequest::parse(
                doc.getCmd(), IDLParserContext("WriteDistributionMetricsCalculator"));
            _addFindAndModifyQuery(opCtx, cmd);
            break;
        }
        case SampledCommandNameEnum::kBulkWrite: {
            auto cmd = BulkWriteCommandRequest::parse(
                doc.getCmd(), IDLParserContext("WriteDistributionMetricsCalculator"));
            _addBulkWriteQuery(opCtx, cmd);
            break;
        }
        default:
            MONGO_UNREACHABLE;
    }
}

void WriteDistributionMetricsCalculator::_addUpdateQuery(
    OperationContext* opCtx,
    const NamespaceString& ns,
    const BSONObj& filter,
    const BSONObj& collation,
    const write_ops::UpdateModification& updateMod,
    bool upsert,
    bool multi,
    const boost::optional<BSONObj>& letParameters,
    const boost::optional<LegacyRuntimeConstants>& runtimeConstants) {
    _numUpdate++;
    auto info =
        _getTargetingInfoForQuery(opCtx, filter, collation, letParameters, runtimeConstants);
    _incrementMetricsForQuery(info, multi);
}

void WriteDistributionMetricsCalculator::_addUpdateQuery(
    OperationContext* opCtx, const write_ops::UpdateCommandRequest& cmd) {
    for (const auto& updateOp : cmd.getUpdates()) {
        _addUpdateQuery(opCtx,
                        cmd.getNamespace(),
                        updateOp.getQ(),
                        write_ops::collationOf(updateOp),
                        updateOp.getU(),
                        updateOp.getUpsert(),
                        updateOp.getMulti(),
                        cmd.getLet(),
                        cmd.getLegacyRuntimeConstants());
    }
}

void WriteDistributionMetricsCalculator::_addDeleteQuery(
    OperationContext* opCtx,
    const BSONObj& filter,
    const BSONObj& collation,
    bool multi,
    const boost::optional<BSONObj>& letParameters,
    const boost::optional<LegacyRuntimeConstants>& runtimeConstants) {
    _numDelete++;
    auto info =
        _getTargetingInfoForQuery(opCtx, filter, collation, letParameters, runtimeConstants);
    _incrementMetricsForQuery(info, multi);
}

void WriteDistributionMetricsCalculator::_addDeleteQuery(
    OperationContext* opCtx, const write_ops::DeleteCommandRequest& cmd) {
    for (const auto& deleteOp : cmd.getDeletes()) {
        _addDeleteQuery(opCtx,
                        deleteOp.getQ(),
                        write_ops::collationOf(deleteOp),
                        deleteOp.getMulti(),
                        cmd.getLet(),
                        cmd.getLegacyRuntimeConstants());
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

void WriteDistributionMetricsCalculator::_addBulkWriteQuery(OperationContext* opCtx,
                                                            const BulkWriteCommandRequest& cmd) {
    const auto& ops = cmd.getOps();
    const auto& nsInfo = cmd.getNsInfo();

    for (const auto& opEntry : ops) {
        auto op = BulkWriteCRUDOp(opEntry);
        auto opType = op.getType();
        if (opType == BulkWriteCRUDOp::kUpdate) {
            auto updateOp = op.getUpdate();
            const auto nsIdx = updateOp->getNsInfoIdx();
            const auto& nsEntry = nsInfo[nsIdx];
            _addUpdateQuery(opCtx,
                            nsEntry.getNs(),
                            updateOp->getFilter(),
                            updateOp->getCollation().get_value_or({}),
                            updateOp->getUpdateMods(),
                            updateOp->getUpsert(),
                            updateOp->getMulti(),
                            cmd.getLet(),
                            /*runtimeConstants=*/boost::none);  // legacyRuntimeConstants are not
                                                                // supported in bulkWrite.
        } else if (opType == BulkWriteCRUDOp::kDelete) {
            auto deleteOp = op.getDelete();
            _addDeleteQuery(opCtx,
                            deleteOp->getFilter(),
                            deleteOp->getCollation().get_value_or({}),
                            deleteOp->getMulti(),
                            cmd.getLet(),
                            /*runtimeConstants=*/boost::none);  // legacyRuntimeConstants are not
                                                                // supported in bulkWrite.
        }
    }
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
