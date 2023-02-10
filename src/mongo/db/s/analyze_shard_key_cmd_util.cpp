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

#include <boost/math/statistics/bivariate_statistics.hpp>

#include "mongo/db/bson/dotted_path_support.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/s/analyze_shard_key_cmd_util.h"
#include "mongo/db/s/analyze_shard_key_read_write_distribution.h"
#include "mongo/db/s/config/initial_split_policy.h"
#include "mongo/db/s/document_source_analyze_shard_key_read_write_distribution.h"
#include "mongo/db/s/range_deletion_task_gen.h"
#include "mongo/db/s/shard_key_index_util.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/factory.h"
#include "mongo/s/analyze_shard_key_documents_gen.h"
#include "mongo/s/analyze_shard_key_server_parameters_gen.h"
#include "mongo/s/analyze_shard_key_util.h"
#include "mongo/s/grid.h"
#include "mongo/s/service_entry_point_mongos.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace analyze_shard_key {

namespace {

constexpr StringData kGroupByKeyFieldName = "key"_sd;
constexpr StringData kNumDocsFieldName = "numDocs"_sd;
constexpr StringData kNumBytesFieldName = "numBytes"_sd;
constexpr StringData kCardinalityFieldName = "cardinality"_sd;
constexpr StringData kFrequencyFieldName = "frequency"_sd;
constexpr StringData kIndexFieldName = "index"_sd;
constexpr StringData kNumOrphanDocsFieldName = "numOrphanDocs"_sd;

const std::vector<double> kPercentiles{0.99, 0.95, 0.9, 0.8, 0.5};

/**
 * Returns an aggregate command request for calculating the cardinality and frequency of the given
 * shard key.
 */
AggregateCommandRequest makeAggregateRequestForCardinalityAndFrequency(
    const NamespaceString& nss, const BSONObj& shardKey, const BSONObj& hintIndexKey) {
    std::vector<BSONObj> pipeline;

    pipeline.push_back(BSON("$project" << BSON("_id" << 0 << kGroupByKeyFieldName
                                                     << BSON("$meta"
                                                             << "indexKey"))));

    BSONObjBuilder groupByBuilder;
    int fieldNum = 0;
    for (const auto& element : shardKey) {
        const auto fieldName = element.fieldNameStringData();
        groupByBuilder.append(kGroupByKeyFieldName + std::to_string(fieldNum),
                              BSON("$getField" << BSON("field" << fieldName << "input"
                                                               << ("$" + kGroupByKeyFieldName))));
        fieldNum++;
    }
    pipeline.push_back(BSON("$group" << BSON("_id" << groupByBuilder.obj() << kFrequencyFieldName
                                                   << BSON("$sum" << 1))));

    pipeline.push_back(BSON("$project" << BSON("_id" << 0)));
    pipeline.push_back(BSON(
        "$setWindowFields" << BSON(
            "sortBy" << BSON(kFrequencyFieldName << 1) << "output"
                     << BSON(kNumDocsFieldName
                             << BSON("$sum" << ("$" + kFrequencyFieldName)) << kCardinalityFieldName
                             << BSON("$sum" << 1) << kIndexFieldName
                             << BSON("$sum" << 1 << "window"
                                            << BSON("documents" << BSON_ARRAY("unbounded"
                                                                              << "current")))))));

    BSONObjBuilder orBuilder;
    BSONArrayBuilder arrayBuilder(orBuilder.subarrayStart("$or"));
    for (const auto& percentile : kPercentiles) {
        arrayBuilder.append(
            BSON("$eq" << BSON_ARRAY(
                     ("$" + kIndexFieldName)
                     << BSON("$ceil" << BSON("$multiply" << BSON_ARRAY(
                                                 percentile << ("$" + kCardinalityFieldName)))))));
    }
    arrayBuilder.done();
    pipeline.push_back(BSON("$match" << BSON("$expr" << orBuilder.done())));

    AggregateCommandRequest aggRequest(nss, pipeline);
    aggRequest.setHint(hintIndexKey);
    aggRequest.setAllowDiskUse(true);
    // Use readConcern "available" to avoid shard filtering since it is expensive.
    aggRequest.setReadConcern(
        BSON(repl::ReadConcernArgs::kLevelFieldName << repl::readConcernLevels::kAvailableName));

    return aggRequest;
}

struct IndexSpec {
    BSONObj keyPattern;
    bool isUnique;
};

/**
 * To be used for finding the index that can be used as a hint for the aggregate command for
 * calculating the cardinality and frequency metrics.
 *
 * Returns the IndexSpec for the index that has the given shard key as a prefix, ignoring the index
 * type (i.e. hashed or range) since the grouping inside the aggregation works with both the
 * original field values and the hashes of the field values. The index must meet the following
 * requirements:
 * - It must have simple collation since that is the only supported collation for shard key string
 *   fields comparisons.
 * - It must not be sparse since such an index omits documents that have null/missing index
 *   key fields.
 * - It must not be partial since such an index omits documents do not match the specified
 *   filter.
 * - It must not be multi-key since a shard key field cannot be an array.
 */
boost::optional<IndexSpec> findCompatiblePrefixedIndex(OperationContext* opCtx,
                                                       const CollectionPtr& collection,
                                                       const IndexCatalog* indexCatalog,
                                                       const BSONObj& shardKey) {
    if (collection->isClustered()) {
        auto indexSpec = collection->getClusteredInfo()->getIndexSpec();
        auto indexKey = indexSpec.getKey();
        if (shardKey.isFieldNamePrefixOf(indexKey)) {
            tassert(6875201, "Expected clustered index to be unique", indexSpec.getUnique());
            return IndexSpec{indexKey, indexSpec.getUnique()};
        }
    }

    auto indexIterator =
        indexCatalog->getIndexIterator(opCtx, IndexCatalog::InclusionPolicy::kReady);
    while (indexIterator->more()) {
        auto indexEntry = indexIterator->next();
        auto indexDesc = indexEntry->descriptor();
        auto indexKey = indexDesc->keyPattern();
        if (indexDesc->collation().isEmpty() && !indexDesc->isSparse() && !indexDesc->isPartial() &&
            !indexEntry->isMultikey(opCtx, collection) && shardKey.isFieldNamePrefixOf(indexKey)) {
            return IndexSpec{indexKey, indexDesc->unique()};
        }
    }

    return boost::none;
}

struct CardinalityFrequencyMetrics {
    int64_t numDocs = 0;
    int64_t cardinality = 0;
    PercentileMetrics frequency;
};

/**
 * Returns the cardinality and frequency metrics for a shard key given that the shard key is unique
 * and the collection has the the given number of documents.
 */
CardinalityFrequencyMetrics calculateCardinalityAndFrequencyUnique(int64_t numDocs) {
    CardinalityFrequencyMetrics metrics;

    metrics.numDocs = numDocs;
    metrics.cardinality = numDocs;
    metrics.frequency.setP99(1);
    metrics.frequency.setP95(1);
    metrics.frequency.setP90(1);
    metrics.frequency.setP80(1);
    metrics.frequency.setP50(1);

    return metrics;
}

/**
 * Returns the cardinality and frequency metrics for the given shard key. Calculates the metrics by
 * running aggregation against the collection. If the shard key is unique, please use the version
 * above since the metrics can be determined without running any aggregations.
 */
CardinalityFrequencyMetrics calculateCardinalityAndFrequencyGeneric(OperationContext* opCtx,
                                                                    const NamespaceString& nss,
                                                                    const BSONObj& shardKey,
                                                                    const BSONObj& hintIndexKey) {
    CardinalityFrequencyMetrics metrics;

    auto aggRequest = makeAggregateRequestForCardinalityAndFrequency(nss, shardKey, hintIndexKey);
    runAggregate(opCtx, aggRequest, [&](const BSONObj& doc) {
        auto numDocs = doc.getField(kNumDocsFieldName).exactNumberLong();
        auto cardinality = doc.getField(kCardinalityFieldName).exactNumberLong();
        auto frequency = doc.getField(kFrequencyFieldName).exactNumberLong();
        auto index = doc.getField(kIndexFieldName).exactNumberLong();

        invariant(numDocs > 0);
        invariant(cardinality > 0);
        invariant(frequency > 0);

        if (metrics.numDocs == 0) {
            metrics.numDocs = numDocs;
        } else {
            invariant(metrics.numDocs == numDocs);
        }

        if (metrics.cardinality == 0) {
            metrics.cardinality = cardinality;
        } else {
            invariant(metrics.cardinality == cardinality);
        }

        if (index == std::ceil(0.99 * cardinality)) {
            metrics.frequency.setP99(frequency);
        }
        if (index == std::ceil(0.95 * cardinality)) {
            metrics.frequency.setP95(frequency);
        }
        if (index == std::ceil(0.9 * cardinality)) {
            metrics.frequency.setP90(frequency);
        }
        if (index == std::ceil(0.8 * cardinality)) {
            metrics.frequency.setP80(frequency);
        }
        if (index == std::ceil(0.5 * cardinality)) {
            metrics.frequency.setP50(frequency);
        }
    });

    uassert(ErrorCodes::InvalidOptions,
            "Cannot analyze the cardinality and frequency of a shard key for an empty collection",
            metrics.numDocs > 0);

    return metrics;
}

/**
 * Returns the monotonicity metrics for the given shard key, i.e. whether the value of the given
 * shard key is monotonically changing in insertion order and the RecordId correlation coefficient
 * calculated by the monotonicity check. If the collection is clustered or the shard key does not
 * have a supporting index, returns 'unknown' and none.
 */
MonotonicityMetrics calculateMonotonicity(OperationContext* opCtx,
                                          const CollectionPtr& collection,
                                          const BSONObj& shardKey) {
    MonotonicityMetrics metrics;

    if (collection->isClustered()) {
        metrics.setType(MonotonicityTypeEnum::kUnknown);
        return metrics;
    }

    if (KeyPattern::isHashedKeyPattern(shardKey) && shardKey.nFields() == 1) {
        metrics.setType(MonotonicityTypeEnum::kNotMonotonic);
        metrics.setRecordIdCorrelationCoefficient(0);
        return metrics;
    }

    auto index = findShardKeyPrefixedIndex(opCtx,
                                           collection,
                                           collection->getIndexCatalog(),
                                           shardKey,
                                           /*requireSingleKey=*/true);

    if (!index) {
        metrics.setType(MonotonicityTypeEnum::kUnknown);
        return metrics;
    }
    // Non-clustered indexes always have an associated IndexDescriptor.
    invariant(index->descriptor());

    std::vector<int64_t> recordIds;
    BSONObj prevKey;

    KeyPattern indexKeyPattern(index->keyPattern());
    auto exec = InternalPlanner::indexScan(opCtx,
                                           &collection,
                                           index->descriptor(),
                                           indexKeyPattern.globalMin(),
                                           indexKeyPattern.globalMax(),
                                           BoundInclusion::kExcludeBothStartAndEndKeys,
                                           PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY);
    try {
        RecordId recordId;
        BSONObj recordVal;
        while (PlanExecutor::ADVANCED == exec->getNext(&recordVal, &recordId)) {
            auto currentKey = dotted_path_support::extractElementsBasedOnTemplate(
                recordVal.replaceFieldNames(shardKey), shardKey);
            if (SimpleBSONObjComparator::kInstance.evaluate(prevKey == currentKey)) {
                continue;
            }
            prevKey = currentKey;
            recordIds.push_back(recordId.getLong());
        }
    } catch (DBException& ex) {
        LOGV2_WARNING(6875301, "Internal error while reading", "ns"_attr = collection->ns());
        ex.addContext("Executor error while reading during 'analyzeShardKey' command");
        throw;
    }

    invariant(recordIds.size() > 0);

    if (recordIds.size() == 1) {
        metrics.setType(MonotonicityTypeEnum::kNotMonotonic);
        metrics.setRecordIdCorrelationCoefficient(0);
        return metrics;
    }

    metrics.setRecordIdCorrelationCoefficient([&] {
        auto& y = recordIds;
        std::vector<int64_t> x(y.size());
        std::iota(x.begin(), x.end(), 1);
        return round(boost::math::statistics::correlation_coefficient<std::vector<int64_t>>(x, y),
                     kMaxNumDecimalPlaces);
    }());
    auto coefficientThreshold = gMonotonicityCorrelationCoefficientThreshold.load();
    LOGV2(6875302,
          "Calculating monotonicity",
          "coefficient"_attr = metrics.getRecordIdCorrelationCoefficient(),
          "coefficientThreshold"_attr = coefficientThreshold);

    metrics.setType(abs(*metrics.getRecordIdCorrelationCoefficient()) >= coefficientThreshold
                        ? MonotonicityTypeEnum::kMonotonic
                        : MonotonicityTypeEnum::kNotMonotonic);

    return metrics;
}

struct CollStatsMetrics {
    int64_t numDocs;
    int64_t avgDocSizeBytes;
    boost::optional<int64_t> numOrphanDocs;
};

/**
 * Returns $collStat metrics for the given collection, i.e. the number of documents, the average
 * document size in bytes and the number of orphan documents if the collection is sharded.
 */
CollStatsMetrics calculateCollStats(OperationContext* opCtx, const NamespaceString& nss) {
    CollStatsMetrics metrics;

    std::vector<BSONObj> pipeline;
    pipeline.push_back(BSON("$collStats" << BSON("storageStats" << BSONObj())));
    pipeline.push_back(BSON("$group" << BSON("_id" << BSONNULL << kNumBytesFieldName
                                                   << BSON("$sum"
                                                           << "$storageStats.size")
                                                   << kNumDocsFieldName
                                                   << BSON("$sum"
                                                           << "$storageStats.count")
                                                   << kNumOrphanDocsFieldName
                                                   << BSON("$sum"
                                                           << "$storageStats.numOrphanDocs"))));
    AggregateCommandRequest aggRequest(nss, pipeline);

    runAggregate(opCtx, aggRequest, [&](const BSONObj& doc) {
        metrics.numDocs = doc.getField(kNumDocsFieldName).exactNumberLong();
        metrics.avgDocSizeBytes =
            doc.getField(kNumBytesFieldName).exactNumberLong() / metrics.numDocs;

        if (serverGlobalParams.clusterRole.isShardRole()) {
            auto cm = uassertStatusOK(
                Grid::get(opCtx)->catalogCache()->getCollectionPlacementInfo(opCtx, nss));

            if (cm.isSharded()) {
                metrics.numOrphanDocs = doc.getField(kNumOrphanDocsFieldName).exactNumberLong();
            }
        }
    });

    return metrics;
}

/**
 * Generates the namespace for the temporary collection storing the split points.
 */
NamespaceString makeSplitPointsNss(const UUID& origCollUuid, const UUID& tempCollUuid) {
    return NamespaceString(NamespaceString::kConfigDb,
                           fmt::format("{}{}.{}",
                                       NamespaceString::kAnalyzeShardKeySplitPointsCollectionPrefix,
                                       origCollUuid.toString(),
                                       tempCollUuid.toString()));
}

/**
 * Generates split points that partition the data for the given collection into N number of ranges
 * with roughly equal number of documents, where N is equal to 'gNumShardKeyRanges', and then
 * persists the split points to a temporary config collection. Returns the namespace for the
 * temporary collection and the afterClusterTime to use in order to find all split point documents
 * (note that this corresponds to the 'operationTime' in the response for the last insert command).
 */
std::pair<NamespaceString, Timestamp> generateSplitPoints(OperationContext* opCtx,
                                                          const NamespaceString& nss,
                                                          const KeyPattern& shardKey) {
    auto origCollUuid = CollectionCatalog::get(opCtx)->lookupUUIDByNSS(opCtx, nss);
    uassert(ErrorCodes::NamespaceNotFound,
            str::stream() << "Cannot analyze a shard key for a non-existing collection",
            origCollUuid);

    auto tempCollUuid = UUID::gen();
    auto splitPointsNss = makeSplitPointsNss(*origCollUuid, tempCollUuid);

    auto shardKeyPattern = ShardKeyPattern(shardKey);
    auto initialSplitter = SamplingBasedSplitPolicy::make(opCtx,
                                                          nss,
                                                          shardKeyPattern,
                                                          gNumShardKeyRanges.load(),
                                                          boost::none,
                                                          gNumSamplesPerShardKeyRange.load());
    const SplitPolicyParams splitParams{tempCollUuid, ShardingState::get(opCtx)->shardId()};
    auto splitPoints = [&] {
        try {
            return initialSplitter.createFirstSplitPoints(opCtx, shardKeyPattern, splitParams);
        } catch (DBException& ex) {
            ex.addContext(str::stream()
                          << "Failed to find split points that partition the data into "
                          << gNumShardKeyRanges.load()
                          << " chunks with roughly equal number of documents using the shard key "
                             "being analyzed");
            throw;
        }
    }();

    Timestamp splitPointsAfterClusterTime;
    auto uassertWriteStatusFn = [&](const BSONObj& resObj) {
        BatchedCommandResponse res;
        std::string errMsg;

        if (!res.parseBSON(resObj, &errMsg)) {
            uasserted(ErrorCodes::FailedToParse, errMsg);
        }

        uassertStatusOK(res.toStatus());
        splitPointsAfterClusterTime = LogicalTime::fromOperationTime(resObj).asTimestamp();
    };

    std::vector<BSONObj> splitPointsToInsert;
    int64_t objSize = 0;

    for (const auto& splitPoint : splitPoints) {
        if (splitPoint.objsize() + objSize >= BSONObjMaxUserSize ||
            splitPointsToInsert.size() >= write_ops::kMaxWriteBatchSize) {
            insertDocuments(opCtx, splitPointsNss, splitPointsToInsert, uassertWriteStatusFn);
            splitPointsToInsert.clear();
        }
        AnalyzeShardKeySplitPointDocument doc;
        doc.setSplitPoint(splitPoint);
        splitPointsToInsert.push_back(doc.toBSON());
        objSize += splitPoint.objsize();
    }
    if (!splitPointsToInsert.empty()) {
        insertDocuments(opCtx, splitPointsNss, splitPointsToInsert, uassertWriteStatusFn);
        splitPointsToInsert.clear();
    }

    invariant(!splitPointsAfterClusterTime.isNull());
    return std::make_pair(splitPointsNss, splitPointsAfterClusterTime);
}

}  // namespace

KeyCharacteristicsMetrics calculateKeyCharacteristicsMetrics(OperationContext* opCtx,
                                                             const NamespaceString& nss,
                                                             const KeyPattern& shardKey) {
    KeyCharacteristicsMetrics metrics;

    auto shardKeyBson = shardKey.toBSON();
    BSONObj indexKeyBson;
    {
        AutoGetCollectionForReadCommand collection(opCtx, nss);

        uassert(ErrorCodes::NamespaceNotFound,
                str::stream() << "Cannot analyze a shard key for a non-existing collection",
                collection);

        uassert(serverGlobalParams.clusterRole == ClusterRole::ShardServer
                    ? ErrorCodes::CollectionIsEmptyLocally
                    : ErrorCodes::InvalidOptions,
                "Cannot analyze a shard key for an empty collection",
                collection->numRecords(opCtx) > 0);

        auto indexSpec = findCompatiblePrefixedIndex(
            opCtx, *collection, collection->getIndexCatalog(), shardKeyBson);

        if (!indexSpec) {
            return {};
        }

        indexKeyBson = indexSpec->keyPattern.getOwned();
        metrics.setIsUnique(shardKeyBson.nFields() == indexKeyBson.nFields() ? indexSpec->isUnique
                                                                             : false);
        auto monotonicityMetrics = calculateMonotonicity(opCtx, *collection, shardKeyBson);
        metrics.setMonotonicity(monotonicityMetrics);
    }

    auto collStatsMetrics = calculateCollStats(opCtx, nss);
    metrics.setAvgDocSizeBytes(collStatsMetrics.avgDocSizeBytes);
    metrics.setNumOrphanDocs(collStatsMetrics.numOrphanDocs);

    auto cardinalityFrequencyMetrics = *metrics.getIsUnique()
        ? calculateCardinalityAndFrequencyUnique(collStatsMetrics.numDocs)
        : calculateCardinalityAndFrequencyGeneric(opCtx, nss, shardKeyBson, indexKeyBson);
    metrics.setNumDocs(cardinalityFrequencyMetrics.numDocs);
    metrics.setNumDistinctValues(cardinalityFrequencyMetrics.cardinality);
    metrics.setFrequency(cardinalityFrequencyMetrics.frequency);

    return metrics;
}

std::pair<ReadDistributionMetrics, WriteDistributionMetrics> calculateReadWriteDistributionMetrics(
    OperationContext* opCtx, const NamespaceString& nss, const KeyPattern& shardKey) {
    ReadDistributionMetrics readDistributionMetrics;
    WriteDistributionMetrics writeDistributionMetrics;

    auto splitPointsShardId = ShardingState::get(opCtx)->shardId();
    auto [splitPointsNss, splitPointsAfterClusterTime] = generateSplitPoints(opCtx, nss, shardKey);

    std::vector<BSONObj> pipeline;
    DocumentSourceAnalyzeShardKeyReadWriteDistributionSpec spec(
        shardKey, splitPointsShardId, splitPointsNss, splitPointsAfterClusterTime);
    pipeline.push_back(
        BSON(DocumentSourceAnalyzeShardKeyReadWriteDistribution::kStageName << spec.toBSON()));
    AggregateCommandRequest aggRequest(nss, pipeline);
    runAggregate(opCtx, nss, aggRequest, [&](const BSONObj& doc) {
        const auto response = DocumentSourceAnalyzeShardKeyReadWriteDistributionResponse::parse(
            IDLParserContext("calculateReadWriteDistributionMetrics"), doc);
        readDistributionMetrics = readDistributionMetrics + response.getReadDistribution();
        writeDistributionMetrics = writeDistributionMetrics + response.getWriteDistribution();
    });

    // Keep only the percentages.
    readDistributionMetrics.setNumSingleShard(boost::none);
    readDistributionMetrics.setNumVariableShard(boost::none);
    readDistributionMetrics.setNumScatterGather(boost::none);
    writeDistributionMetrics.setNumSingleShard(boost::none);
    writeDistributionMetrics.setNumVariableShard(boost::none);
    writeDistributionMetrics.setNumScatterGather(boost::none);
    writeDistributionMetrics.setNumShardKeyUpdates(boost::none);
    writeDistributionMetrics.setNumSingleWritesWithoutShardKey(boost::none);
    writeDistributionMetrics.setNumMultiWritesWithoutShardKey(boost::none);

    dropCollection(opCtx, splitPointsNss);

    return std::make_pair(readDistributionMetrics, writeDistributionMetrics);
}

}  // namespace analyze_shard_key
}  // namespace mongo
