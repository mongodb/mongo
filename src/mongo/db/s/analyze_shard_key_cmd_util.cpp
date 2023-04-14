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
#include "mongo/db/catalog/collection_uuid_mismatch_info.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/process_interface/shardsvr_process_interface.h"
#include "mongo/db/pipeline/sharded_agg_helpers.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
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
#include "mongo/s/query_analysis_client.h"
#include "mongo/s/service_entry_point_mongos.h"
#include "mongo/s/stale_shard_version_helpers.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {
namespace analyze_shard_key {

namespace {

MONGO_FAIL_POINT_DEFINE(analyzeShardKeyPauseBeforeCalculatingKeyCharacteristicsMetrics);
MONGO_FAIL_POINT_DEFINE(analyzeShardKeyPauseBeforeCalculatingReadWriteDistributionMetrics);

constexpr StringData kIndexKeyFieldName = "key"_sd;
constexpr StringData kDocFieldName = "doc"_sd;
constexpr StringData kNumDocsFieldName = "numDocs"_sd;
constexpr StringData kNumBytesFieldName = "numBytes"_sd;
constexpr StringData kNumDistinctValuesFieldName = "numDistinctValues"_sd;
constexpr StringData kFrequencyFieldName = "frequency"_sd;
constexpr StringData kNumOrphanDocsFieldName = "numOrphanDocs"_sd;

const int64_t kEmptyDocSizeBytes = BSONObj().objsize();

/**
 * Returns an aggregate command request for calculating the cardinality and frequency metrics for
 * the given shard key.
 *
 * If the hint index is a hashed index and the shard key contains the hashed field, the aggregation
 * will return documents of the following format, where 'doc' is a document whose shard key value
 * has the attached 'frequency'.
 *   {
 *      doc: <object>
 *      frequency: <integer>
 *      numDocs: <integer>
 *      numDistinctValues: <integer>
 *   }
 * Otherwise, the aggregation will return documents of the following format, where 'key' is the
 * hint index value for the shard key value that has the attached 'frequency'.
 *   {
 *      key: <object>
 *      frequency: <integer>
 *      numDocs: <integer>
 *      numDistinctValues: <integer>
 *   }
 * The former case involves an additional FETCH for every document returned since it needs to look
 * up a document from the index value.
 */
AggregateCommandRequest makeAggregateRequestForCardinalityAndFrequency(const NamespaceString& nss,
                                                                       const BSONObj& shardKey,
                                                                       const BSONObj& hintIndexKey,
                                                                       int numMostCommonValues) {
    uassertStatusOK(validateIndexKey(hintIndexKey));

    std::vector<BSONObj> pipeline;

    pipeline.push_back(BSON("$project" << BSON("_id" << 0 << kIndexKeyFieldName
                                                     << BSON("$meta"
                                                             << "indexKey"))));

    BSONObjBuilder groupByBuilder;
    int fieldNum = 0;
    boost::optional<std::string> origHashedFieldName;
    boost::optional<std::string> tempHashedFieldName;
    StringMap<std::string> origToTempFieldName;
    for (const auto& element : shardKey) {
        // Use a temporary field name since it is invalid to group by a field name that contains
        // dots.
        const auto origFieldName = element.fieldNameStringData();
        const auto tempFieldName = kIndexKeyFieldName + std::to_string(fieldNum);
        groupByBuilder.append(tempFieldName,
                              BSON("$getField" << BSON("field" << origFieldName << "input"
                                                               << ("$" + kIndexKeyFieldName))));
        if (ShardKeyPattern::isHashedPatternEl(hintIndexKey.getField(origFieldName))) {
            origHashedFieldName.emplace(origFieldName);
            tempHashedFieldName.emplace(tempFieldName);
        }
        origToTempFieldName.emplace(origFieldName, tempFieldName);
        fieldNum++;
    }
    pipeline.push_back(BSON("$group" << BSON("_id" << groupByBuilder.obj() << kFrequencyFieldName
                                                   << BSON("$sum" << 1))));

    pipeline.push_back(BSON("$setWindowFields"
                            << BSON("sortBy"
                                    << BSON(kFrequencyFieldName << -1) << "output"
                                    << BSON(kNumDocsFieldName
                                            << BSON("$sum" << ("$" + kFrequencyFieldName))
                                            << kNumDistinctValuesFieldName << BSON("$sum" << 1)))));

    pipeline.push_back(BSON("$limit" << numMostCommonValues));

    if (origHashedFieldName) {
        invariant(tempHashedFieldName);

        BSONObjBuilder letBuilder;
        BSONObjBuilder matchBuilder;
        BSONArrayBuilder matchArrayBuilder(matchBuilder.subarrayStart("$and"));
        for (const auto& [origFieldName, tempFieldName] : origToTempFieldName) {
            letBuilder.append(tempFieldName, ("$_id." + tempFieldName));
            auto eqArray = (origFieldName == *origHashedFieldName)
                ? BSON_ARRAY(BSON("$toHashedIndexKey" << ("$" + *origHashedFieldName))
                             << ("$$" + tempFieldName))
                : BSON_ARRAY(("$" + origFieldName) << ("$$" + tempFieldName));
            matchArrayBuilder.append(BSON("$expr" << BSON("$eq" << eqArray)));
        }
        matchArrayBuilder.done();

        pipeline.push_back(BSON(
            "$lookup" << BSON(
                "from" << nss.coll().toString() << "let" << letBuilder.obj() << "pipeline"
                       << BSON_ARRAY(BSON("$match" << matchBuilder.obj()) << BSON("$limit" << 1))
                       << "as"
                       << "docs")));
        pipeline.push_back(BSON("$set" << BSON(kDocFieldName << BSON("$first"
                                                                     << "$docs"))));
        pipeline.push_back(BSON("$unset"
                                << "docs"));
    } else {
        pipeline.push_back(BSON("$set" << BSON(kIndexKeyFieldName << "$_id")));
    }
    pipeline.push_back(BSON("$unset"
                            << "_id"));

    AggregateCommandRequest aggRequest(nss, pipeline);
    aggRequest.setHint(hintIndexKey);
    aggRequest.setAllowDiskUse(true);
    // (re)shardCollection by design uses simple collation for comparing shard key values.
    aggRequest.setCollation(CollationSpec::kSimpleSpec);
    // Use readConcern "available" to avoid shard filtering since it is expensive.
    aggRequest.setReadConcern(
        BSON(repl::ReadConcernArgs::kLevelFieldName << repl::readConcernLevels::kAvailableName));

    return aggRequest;
}

/**
 * Runs the aggregate command 'aggRequest' locally and getMore commands for it if needed and
 * applies 'callbackFn' to each returned document.
 */
void runLocalAggregate(OperationContext* opCtx,
                       AggregateCommandRequest aggRequest,
                       std::function<void(const BSONObj&)> callbackFn) {
    DBDirectClient client(opCtx);
    auto cursor = uassertStatusOK(DBClientCursor::fromAggregationRequest(
        &client, aggRequest, true /* secondaryOk */, false /* useExhaust*/));

    while (cursor->more()) {
        auto doc = cursor->next();
        callbackFn(doc);
    }
}

/**
 * Runs the aggregate command 'aggRequest' on the shards that the query targets and getMore
 * commands for it if needed, and applies 'callbackFn' to each returned document. On a sharded
 * cluster, internally retries on shard versioning errors.
 */
void runClusterAggregate(OperationContext* opCtx,
                         AggregateCommandRequest aggRequest,
                         std::function<void(const BSONObj&)> callbackFn) {
    invariant(serverGlobalParams.clusterRole.has(ClusterRole::ShardServer));

    auto nss = aggRequest.getNamespace();
    boost::optional<UUID> collUuid;
    std::unique_ptr<CollatorInterface> collation;
    {
        AutoGetCollectionForReadCommandMaybeLockFree collection(opCtx, nss);

        uassert(ErrorCodes::NamespaceNotFound,
                str::stream() << "Cannot run aggregation against a non-existing collection",
                collection);

        collUuid = collection->uuid();
        collation = [&] {
            auto collationObj = aggRequest.getCollation();
            if (collationObj && !collationObj->isEmpty()) {
                return uassertStatusOK(CollatorFactoryInterface::get(opCtx->getServiceContext())
                                           ->makeFromBSON(*collationObj));
            }
            return CollatorInterface::cloneCollator(collection->getDefaultCollator());
        }();
    }

    StringMap<ExpressionContext::ResolvedNamespace> resolvedNamespaces;
    resolvedNamespaces[nss.coll()] = {nss, std::vector<BSONObj>{}};

    auto pi = std::make_shared<ShardServerProcessInterface>(
        Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor());

    auto expCtx = make_intrusive<ExpressionContext>(opCtx,
                                                    boost::none, /* explain */
                                                    false,       /* fromMongos */
                                                    false,       /* needsMerge */
                                                    aggRequest.getAllowDiskUse(),
                                                    true,  /* bypassDocumentValidation */
                                                    false, /* isMapReduceCommand */
                                                    nss,
                                                    boost::none, /* runtimeConstants */
                                                    std::move(collation),
                                                    std::move(pi),
                                                    std::move(resolvedNamespaces),
                                                    collUuid);
    expCtx->tempDir = storageGlobalParams.dbpath + "/_tmp";

    auto pipeline =
        shardVersionRetry(opCtx, Grid::get(opCtx)->catalogCache(), nss, "AnalyzeShardKey"_sd, [&] {
            return Pipeline::makePipeline(aggRequest, expCtx);
        });

    while (auto doc = pipeline->getNext()) {
        callbackFn(doc->toBson());
    }
}

/**
 * Runs the aggregate command 'aggRequest' and getMore commands for it if needed, and applies
 * 'callbackFn' to each returned document. On a sharded cluster, internally retries on shard
 * versioning errors.
 */
void runAggregate(OperationContext* opCtx,
                  AggregateCommandRequest aggRequest,
                  std::function<void(const BSONObj&)> callbackFn) {
    if (serverGlobalParams.clusterRole.has(ClusterRole::ShardServer)) {
        return runClusterAggregate(opCtx, aggRequest, callbackFn);
    }
    return runLocalAggregate(opCtx, aggRequest, callbackFn);
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

    // Go through the indexes in the index catalog to find the most compatible index.
    boost::optional<IndexSpec> compatibleIndexSpec;

    auto indexIterator =
        indexCatalog->getIndexIterator(opCtx, IndexCatalog::InclusionPolicy::kReady);
    while (indexIterator->more()) {
        auto indexEntry = indexIterator->next();
        auto indexDesc = indexEntry->descriptor();
        auto indexKey = indexDesc->keyPattern();

        if (indexDesc->getIndexType() != IndexType::INDEX_BTREE &&
            indexDesc->getIndexType() != IndexType::INDEX_HASHED) {
            continue;
        }
        if (indexEntry->isMultikey(opCtx, collection)) {
            continue;
        }
        if (indexDesc->isSparse() || indexDesc->isPartial()) {
            continue;
        }
        if (!indexDesc->collation().isEmpty()) {
            continue;
        }

        if (!shardKey.isFieldNamePrefixOf(indexKey)) {
            continue;
        }
        if (!compatibleIndexSpec.has_value() ||
            compatibleIndexSpec->keyPattern.nFields() > indexKey.nFields() ||
            (!compatibleIndexSpec->isUnique && indexDesc->unique())) {
            // Give preference to indexes with fewer fields and unique indexes since they can help
            // us infer if the shard key is unique.
            compatibleIndexSpec = IndexSpec{indexKey, indexDesc->unique()};
        }
    }

    return compatibleIndexSpec;
}

struct CardinalityFrequencyMetrics {
    int64_t numDocs = 0;
    int64_t numDistinctValues = 0;
    std::vector<ValueFrequencyMetrics> mostCommonValues;
};

// The size limit for the most common values. Leave some padding for other fields in the response.
constexpr int kMaxBSONObjSizeMostCommonValues = BSONObjMaxUserSize - 1000 * 1024;

/**
 * Creates a BSONObj by appending the fields in 'obj' to it. Upon encoutering a field whose size
 * exceeds the remaining size, truncates it by setting its value to a BSONObj with a "type" field
 * and a "sizeBytes" field. If the field is of type of Object and the depth is 0, recurse to
 * truncate the subfields instead.
 */
BSONObj truncateBSONObj(const BSONObj& obj, int maxSize, int depth = 0) {
    BSONObjBuilder bob;
    auto getRemainingSize = [&] {
        auto remaining = maxSize - bob.bb().len();
        tassert(7477401,
                str::stream() << "Failed to truncate BSON object of size " << obj.objsize(),
                remaining > 0);
        return remaining;
    };

    BSONObjIterator it(obj);
    while (it.more()) {
        auto remainingSize = getRemainingSize();
        auto element = it.next();
        if (element.size() < remainingSize) {
            bob.append(element);
        } else {
            auto fieldName = element.fieldName();
            if (element.type() == BSONType::Object && depth == 0) {
                auto fieldValue = truncateBSONObj(element.Obj(), remainingSize, depth + 1);
                bob.append(fieldName, fieldValue);
            } else {
                bob.append(fieldName,
                           BSON("type" << typeName(element.type()) << "value"
                                       << "truncated"
                                       << "sizeBytes" << element.valuesize()));
            }
        }
    }
    return bob.obj();
}

/**
 * Returns the cardinality and frequency metrics for a shard key given that the shard key is unique
 * and the collection has the the given fast count of the number of documents.
 */
CardinalityFrequencyMetrics calculateCardinalityAndFrequencyUnique(OperationContext* opCtx,
                                                                   const NamespaceString& nss,
                                                                   const BSONObj& shardKey,
                                                                   int64_t numDocs) {
    LOGV2(6915302,
          "Calculating cardinality and frequency for a unique shard key",
          logAttrs(nss),
          "shardKey"_attr = shardKey);

    CardinalityFrequencyMetrics metrics;

    const auto numMostCommonValues = gNumMostCommonValues.load();
    const auto maxSizeBytesPerValue = kMaxBSONObjSizeMostCommonValues / numMostCommonValues;

    std::vector<BSONObj> pipeline;
    pipeline.push_back(BSON("$match" << BSONObj()));
    pipeline.push_back(BSON("$limit" << numMostCommonValues));
    AggregateCommandRequest aggRequest(nss, pipeline);
    aggRequest.setReadConcern(extractReadConcern(opCtx));

    runAggregate(opCtx, aggRequest, [&](const BSONObj& doc) {
        auto value = dotted_path_support::extractElementsBasedOnTemplate(doc.getOwned(), shardKey);
        if (value.objsize() > maxSizeBytesPerValue) {
            value = truncateBSONObj(value, maxSizeBytesPerValue);
        }
        metrics.mostCommonValues.emplace_back(std::move(value), 1);
    });

    metrics.numDistinctValues = [&] {
        if (int64_t numMostCommonValues = metrics.mostCommonValues.size();
            numDocs < numMostCommonValues) {
            LOGV2_WARNING(
                7477402,
                "The number of documents returned by $collStats appears to be less than the number "
                "of sampled documents for cardinality and frequency metrics calculation. This is "
                "likely caused by an unclean shutdown that resulted in an inaccurate fast count "
                "or by insertions that have occurred since the command started. Setting the number "
                "of documents to the number of sampled documents.",
                "numCountedDocs"_attr = numDocs,
                "numSampledDocs"_attr = numMostCommonValues);
            return numMostCommonValues;
        }
        return numDocs;
    }();
    metrics.numDocs = metrics.numDistinctValues;

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
    LOGV2(6915303,
          "Calculating cardinality and frequency for a non-unique shard key",
          logAttrs(nss),
          "shardKey"_attr = shardKey,
          "indexKey"_attr = hintIndexKey);

    CardinalityFrequencyMetrics metrics;

    const auto numMostCommonValues = gNumMostCommonValues.load();
    const auto maxSizeBytesPerValue = kMaxBSONObjSizeMostCommonValues / numMostCommonValues;

    auto aggRequest = makeAggregateRequestForCardinalityAndFrequency(
        nss, shardKey, hintIndexKey, numMostCommonValues);

    runAggregate(opCtx, aggRequest, [&](const BSONObj& doc) {
        auto numDocs = doc.getField(kNumDocsFieldName).exactNumberLong();
        invariant(numDocs > 0);
        if (metrics.numDocs == 0) {
            metrics.numDocs = numDocs;
        } else {
            invariant(metrics.numDocs == numDocs);
        }

        auto numDistinctValues = doc.getField(kNumDistinctValuesFieldName).exactNumberLong();
        invariant(numDistinctValues > 0);
        if (metrics.numDistinctValues == 0) {
            metrics.numDistinctValues = numDistinctValues;
        } else {
            invariant(metrics.numDistinctValues == numDistinctValues);
        }

        auto value = [&] {
            if (doc.hasField(kDocFieldName)) {
                return dotted_path_support::extractElementsBasedOnTemplate(
                    doc.getObjectField(kDocFieldName), shardKey);
            } else if (doc.hasField(kIndexKeyFieldName)) {
                return dotted_path_support::extractElementsBasedOnTemplate(
                    doc.getObjectField(kIndexKeyFieldName).replaceFieldNames(shardKey), shardKey);
            } else
                uasserted(7588600,
                          str::stream() << "Found a document with unexpected format " << doc);
        }();
        if (value.objsize() > maxSizeBytesPerValue) {
            value = truncateBSONObj(value, maxSizeBytesPerValue);
        }
        auto frequency = doc.getField(kFrequencyFieldName).exactNumberLong();
        invariant(frequency > 0);
        metrics.mostCommonValues.emplace_back(std::move(value), frequency);
    });

    uassert(ErrorCodes::IllegalOperation,
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
    LOGV2(6915304,
          "Calculating monotonicity",
          logAttrs(collection->ns()),
          "shardKey"_attr = shardKey);

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

    const auto index = findShardKeyPrefixedIndex(opCtx,
                                                 collection,
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
    int64_t numKeys = 0;

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
            recordIds.push_back(recordId.getLong());
            auto currentKey = dotted_path_support::extractElementsBasedOnTemplate(
                recordVal.replaceFieldNames(shardKey), shardKey);
            if (SimpleBSONObjComparator::kInstance.evaluate(prevKey != currentKey)) {
                prevKey = currentKey;
                numKeys++;
            }
        }
    } catch (DBException& ex) {
        LOGV2_WARNING(6875301, "Internal error while reading", "ns"_attr = collection->ns());
        ex.addContext("Executor error while reading during 'analyzeShardKey' command");
        throw;
    }

    uassert(serverGlobalParams.clusterRole.has(ClusterRole::ShardServer)
                ? ErrorCodes::CollectionIsEmptyLocally
                : ErrorCodes::IllegalOperation,
            "Cannot analyze the monotonicity of a shard key for an empty collection",
            recordIds.size() > 0);

    if (numKeys == 1) {
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
          "Calculated monotonicity",
          logAttrs(collection->ns()),
          "shardKey"_attr = shardKey,
          "indexKey"_attr = indexKeyPattern,
          "numRecords"_attr = recordIds.size(),
          "coefficient"_attr = metrics.getRecordIdCorrelationCoefficient(),
          "coefficientThreshold"_attr = coefficientThreshold);

    metrics.setType(abs(*metrics.getRecordIdCorrelationCoefficient()) >= coefficientThreshold
                        ? MonotonicityTypeEnum::kMonotonic
                        : MonotonicityTypeEnum::kNotMonotonic);

    return metrics;
}

struct CollStatsMetrics {
    int64_t numDocs = 0;
    int64_t avgDocSizeBytes = 0;
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
    aggRequest.setReadConcern(extractReadConcern(opCtx));

    auto isShardedCollection = [&] {
        if (serverGlobalParams.clusterRole.has(ClusterRole::ShardServer)) {
            auto cm = uassertStatusOK(
                          Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfo(opCtx, nss))
                          .cm;
            return cm.isSharded();
        }
        return false;
    }();

    runAggregate(opCtx, aggRequest, [&](const BSONObj& doc) {
        metrics.numDocs = doc.getField(kNumDocsFieldName).exactNumberLong();
        if (metrics.numDocs == 0) {
            LOGV2_WARNING(
                7477403,
                "The number of documents returned by $collStats indicates that the collection is "
                "empty. This is likely caused by an unclean shutdown that resulted in an "
                "inaccurate fast count or by deletions that have occurred since the command "
                "started.");
            metrics.avgDocSizeBytes = 0;
            if (isShardedCollection) {
                metrics.numOrphanDocs = 0;
            }
            return;
        }

        metrics.avgDocSizeBytes =
            doc.getField(kNumBytesFieldName).exactNumberLong() / metrics.numDocs;
        if (isShardedCollection) {
            metrics.numOrphanDocs = doc.getField(kNumOrphanDocsFieldName).exactNumberLong();
        }
    });

    return metrics;
}

/**
 * Generates split points that partition the data for the given collection into N number of ranges
 * with roughly equal number of documents, where N is equal to 'gNumShardKeyRanges', and then
 * persists the split points to the config collection for storing split point documents. Returns
 * the filter and the afterClusterTime to use when fetching the split point documents, where the
 * latter corresponds to the 'operationTime' in the response for the last insert command.
 */
std::pair<BSONObj, Timestamp> generateSplitPoints(OperationContext* opCtx,
                                                  const NamespaceString& nss,
                                                  const UUID& collUuid,
                                                  const KeyPattern& shardKey) {
    auto origCollUuid = CollectionCatalog::get(opCtx)->lookupUUIDByNSS(opCtx, nss);
    uassert(ErrorCodes::NamespaceNotFound,
            str::stream() << "Cannot analyze a shard key for a non-existing collection",
            origCollUuid);
    // Perform best-effort validation that the collection has not been dropped and recreated.
    uassert(CollectionUUIDMismatchInfo(
                DatabaseName{nss.db()}, collUuid, nss.coll().toString(), boost::none),
            str::stream() << "Found that the collection UUID has changed from " << collUuid
                          << " to " << origCollUuid << " since the command started",
            origCollUuid == collUuid);

    auto commandId = UUID::gen();
    LOGV2(7559400,
          "Generating split points using the shard key being analyzed",
          logAttrs(nss),
          "shardKey"_attr = shardKey,
          "commandId"_attr = commandId);

    auto tempCollUuid = UUID::gen();
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

        if (res.isErrDetailsSet() && res.sizeErrDetails() > 0) {
            for (const auto& err : res.getErrDetails()) {
                if (err.getStatus() == ErrorCodes::DuplicateKey) {
                    LOGV2(7433800, "Ignoring insert error", "error"_attr = redact(err.getStatus()));
                    continue;
                }
                uassertStatusOK(err.getStatus());
            }
        } else {
            uassertStatusOK(res.toStatus());
        }

        splitPointsAfterClusterTime = LogicalTime::fromOperationTime(resObj).asTimestamp();
    };

    std::vector<BSONObj> splitPointsToInsert;
    int64_t objSize = 0;

    auto expireAt = opCtx->getServiceContext()->getFastClockSource()->now() +
        mongo::Milliseconds(gAnalyzeShardKeySplitPointExpirationSecs.load() * 1000);
    for (const auto& splitPoint : splitPoints) {
        // Performs best-effort validation again that the shard key does not contain an array field
        // by asserting that split point does not contain an array field.
        uassertShardKeyValueNotContainArrays(splitPoint);

        if (splitPoint.objsize() + objSize >= BSONObjMaxUserSize ||
            splitPointsToInsert.size() >= write_ops::kMaxWriteBatchSize) {
            QueryAnalysisClient::get(opCtx).insert(
                opCtx,
                NamespaceString::kConfigAnalyzeShardKeySplitPointsNamespace,
                splitPointsToInsert,
                uassertWriteStatusFn);
            splitPointsToInsert.clear();
        }
        AnalyzeShardKeySplitPointDocument doc;
        doc.setId({commandId, UUID::gen() /* splitPointId */});
        doc.setNs(nss);
        doc.setSplitPoint(splitPoint);
        doc.setExpireAt(expireAt);
        splitPointsToInsert.push_back(doc.toBSON());
        objSize += splitPoint.objsize();
    }
    if (!splitPointsToInsert.empty()) {
        QueryAnalysisClient::get(opCtx).insert(
            opCtx,
            NamespaceString::kConfigAnalyzeShardKeySplitPointsNamespace,
            splitPointsToInsert,
            uassertWriteStatusFn);
        splitPointsToInsert.clear();
    }

    invariant(!splitPointsAfterClusterTime.isNull());
    auto splitPointsFilter = BSON((AnalyzeShardKeySplitPointDocument::kIdFieldName + "." +
                                   AnalyzeShardKeySplitPointId::kCommandIdFieldName)
                                  << commandId);
    return {std::move(splitPointsFilter), splitPointsAfterClusterTime};
}

}  // namespace

KeyCharacteristicsMetrics calculateKeyCharacteristicsMetrics(OperationContext* opCtx,
                                                             const NamespaceString& nss,
                                                             const UUID& collUuid,
                                                             const KeyPattern& shardKey) {
    KeyCharacteristicsMetrics metrics;

    auto shardKeyBson = shardKey.toBSON();
    BSONObj indexKeyBson;
    {
        AutoGetCollectionForReadCommandMaybeLockFree collection(opCtx, nss);

        uassert(ErrorCodes::NamespaceNotFound,
                str::stream() << "Cannot analyze a shard key for a non-existing collection",
                collection);
        // Perform best-effort validation that the collection has not been dropped and recreated.
        uassert(CollectionUUIDMismatchInfo(
                    DatabaseName{nss.db()}, collUuid, nss.coll().toString(), boost::none),
                str::stream() << "Found that the collection UUID has changed from " << collUuid
                              << " to " << collection->uuid() << " since the command started",
                collection->uuid() == collUuid);

        // Performs best-effort validation that the shard key does not contain an array field by
        // extracting the shard key value from a random document in the collection and asserting
        // that it does not contain an array field.

        // Save the original readConcern since the one on the opCtx will get overwritten as part
        // running a command via DBDirectClient below, and it is illegal to specify readConcern
        // to DBDirectClient::find().
        auto originalReadConcernArgs = repl::ReadConcernArgs::get(opCtx);

        DBDirectClient client(opCtx);
        auto doc = client.findOne(nss, {});
        uassert(serverGlobalParams.clusterRole.has(ClusterRole::ShardServer)
                    ? ErrorCodes::CollectionIsEmptyLocally
                    : ErrorCodes::IllegalOperation,
                "Cannot analyze the characteristics of a shard key for an empty collection",
                !doc.isEmpty());
        auto value = dotted_path_support::extractElementsBasedOnTemplate(doc, shardKeyBson);
        uassertShardKeyValueNotContainArrays(value);

        // Restore the original readConcern.
        repl::ReadConcernArgs::get(opCtx) = originalReadConcernArgs;

        auto indexSpec = findCompatiblePrefixedIndex(
            opCtx, *collection, collection->getIndexCatalog(), shardKeyBson);

        if (!indexSpec) {
            return {};
        }

        indexKeyBson = indexSpec->keyPattern.getOwned();

        LOGV2(6915305,
              "Calculating metrics about the characteristics of the shard key",
              logAttrs(nss),
              "shardKey"_attr = shardKeyBson,
              "indexKey"_attr = indexKeyBson);
        analyzeShardKeyPauseBeforeCalculatingKeyCharacteristicsMetrics.pauseWhileSet(opCtx);

        metrics.setIsUnique(shardKeyBson.nFields() == indexKeyBson.nFields() ? indexSpec->isUnique
                                                                             : false);
        auto monotonicityMetrics = calculateMonotonicity(opCtx, *collection, shardKeyBson);
        metrics.setMonotonicity(monotonicityMetrics);
    }

    auto collStatsMetrics = calculateCollStats(opCtx, nss);
    auto cardinalityFrequencyMetrics = *metrics.getIsUnique()
        ? calculateCardinalityAndFrequencyUnique(opCtx, nss, shardKeyBson, collStatsMetrics.numDocs)
        : calculateCardinalityAndFrequencyGeneric(opCtx, nss, shardKeyBson, indexKeyBson);

    metrics.setNumDocs(cardinalityFrequencyMetrics.numDocs);
    metrics.setNumDistinctValues(cardinalityFrequencyMetrics.numDistinctValues);
    metrics.setMostCommonValues(cardinalityFrequencyMetrics.mostCommonValues);
    // The average document size returned by $collStats can be inaccurate (or even zero) if there
    // has been an unclean shutdown since that can result in inaccurate fast data statistics. To
    // avoid nonsensical metrics, if the collection is not empty, specify the lower limit for the
    // average document size to the size of an empty document.
    metrics.setAvgDocSizeBytes(cardinalityFrequencyMetrics.numDocs > 0
                                   ? std::max(kEmptyDocSizeBytes, collStatsMetrics.avgDocSizeBytes)
                                   : 0);
    metrics.setNumOrphanDocs(collStatsMetrics.numOrphanDocs);

    return metrics;
}

std::pair<ReadDistributionMetrics, WriteDistributionMetrics> calculateReadWriteDistributionMetrics(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const UUID& collUuid,
    const KeyPattern& shardKey) {
    LOGV2(6915306,
          "Calculating metrics about the read and write distribution",
          logAttrs(nss),
          "shardKey"_attr = shardKey);
    analyzeShardKeyPauseBeforeCalculatingReadWriteDistributionMetrics.pauseWhileSet(opCtx);

    ReadDistributionMetrics readDistributionMetrics;
    WriteDistributionMetrics writeDistributionMetrics;

    auto [splitPointsFilter, splitPointsAfterClusterTime] =
        generateSplitPoints(opCtx, nss, collUuid, shardKey);

    std::vector<BSONObj> pipeline;
    DocumentSourceAnalyzeShardKeyReadWriteDistributionSpec spec(
        shardKey, splitPointsFilter, splitPointsAfterClusterTime);
    if (ShardingState::get(opCtx)->enabled()) {
        spec.setSplitPointsShardId(ShardingState::get(opCtx)->shardId());
    }
    pipeline.push_back(
        BSON(DocumentSourceAnalyzeShardKeyReadWriteDistribution::kStageName << spec.toBSON()));
    AggregateCommandRequest aggRequest(nss, pipeline);
    aggRequest.setReadConcern(extractReadConcern(opCtx));

    runAggregate(opCtx, aggRequest, [&](const BSONObj& doc) {
        const auto response = DocumentSourceAnalyzeShardKeyReadWriteDistributionResponse::parse(
            IDLParserContext("calculateReadWriteDistributionMetrics"), doc);
        readDistributionMetrics = readDistributionMetrics + response.getReadDistribution();
        writeDistributionMetrics = writeDistributionMetrics + response.getWriteDistribution();
    });

    // Keep only the percentages.
    readDistributionMetrics.setNumSingleShard(boost::none);
    readDistributionMetrics.setNumMultiShard(boost::none);
    readDistributionMetrics.setNumScatterGather(boost::none);
    writeDistributionMetrics.setNumSingleShard(boost::none);
    writeDistributionMetrics.setNumMultiShard(boost::none);
    writeDistributionMetrics.setNumScatterGather(boost::none);
    writeDistributionMetrics.setNumShardKeyUpdates(boost::none);
    writeDistributionMetrics.setNumSingleWritesWithoutShardKey(boost::none);
    writeDistributionMetrics.setNumMultiWritesWithoutShardKey(boost::none);

    return std::make_pair(readDistributionMetrics, writeDistributionMetrics);
}

}  // namespace analyze_shard_key
}  // namespace mongo
