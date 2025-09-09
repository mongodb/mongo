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

#include "mongo/db/s/analyze_shard_key_cmd_util.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/dotted_path/dotted_path_support.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/bson/timestamp.h"
#include "mongo/bson/util/builder.h"
#include "mongo/client/dbclient_cursor.h"
#include "mongo/db/client.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/exec/agg/pipeline_builder.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/global_catalog/catalog_cache/catalog_cache.h"
#include "mongo/db/global_catalog/chunk_manager.h"
#include "mongo/db/global_catalog/ddl/shard_key_index_util.h"
#include "mongo/db/global_catalog/shard_key_pattern.h"
#include "mongo/db/global_catalog/type_tags.h"
#include "mongo/db/index_names.h"
#include "mongo/db/local_catalog/clustered_collection_options_gen.h"
#include "mongo/db/local_catalog/collection_uuid_mismatch_info.h"
#include "mongo/db/local_catalog/index_catalog.h"
#include "mongo/db/local_catalog/index_catalog_entry.h"
#include "mongo/db/local_catalog/index_descriptor.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/process_interface/shardsvr_process_interface.h"
#include "mongo/db/query/collation/collation_spec.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/query/write_ops/write_ops.h"
#include "mongo/db/query/write_ops/write_ops_parsers.h"
#include "mongo/db/record_id.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/s/analyze_shard_key_read_write_distribution.h"
#include "mongo/db/s/analyze_shard_key_util.h"
#include "mongo/db/s/config/initial_split_policy.h"
#include "mongo/db/s/document_source_analyze_shard_key_read_write_distribution.h"
#include "mongo/db/s/document_source_analyze_shard_key_read_write_distribution_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/random.h"
#include "mongo/s/analyze_shard_key_documents_gen.h"
#include "mongo/s/analyze_shard_key_server_parameters_gen.h"
#include "mongo/s/query_analysis_client.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/str.h"
#include "mongo/util/string_map.h"
#include "mongo/util/time_support.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <memory>
#include <numeric>
#include <set>
#include <string>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <boost/cstdint.hpp>
#include <boost/math/statistics/bivariate_statistics.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {
namespace analyze_shard_key {

namespace {

MONGO_FAIL_POINT_DEFINE(analyzeShardKeyPauseBeforeCalculatingKeyCharacteristicsMetrics);
MONGO_FAIL_POINT_DEFINE(analyzeShardKeyPauseBeforeCalculatingReadWriteDistributionMetrics);
MONGO_FAIL_POINT_DEFINE(analyzeShardKeyPauseBeforeCalculatingCollStatsMetrics);
MONGO_FAIL_POINT_DEFINE(analyzeShardKeyHangInClusterAggregate);

constexpr StringData kIndexKeyFieldName = "key"_sd;
constexpr StringData kDocFieldName = "doc"_sd;
constexpr StringData kNumDocsFieldName = "numDocs"_sd;
constexpr StringData kNumBytesFieldName = "numBytes"_sd;
constexpr StringData kNumDistinctValuesFieldName = "numDistinctValues"_sd;
constexpr StringData kMostCommonValuesFieldName = "mostCommonValues"_sd;
constexpr StringData kFrequencyFieldName = "frequency"_sd;
constexpr StringData kNumOrphanDocsFieldName = "numOrphanDocs"_sd;

const std::string kOrphanDocsWarningMessage =
    "Due to performance reasons, the analyzeShardKey command does not filter out orphan documents "
    "when calculating metrics about the characteristics of the shard key. Therefore, if \"" +
    KeyCharacteristicsMetrics::kNumOrphanDocsFieldName + "\" is large relative to \"" +
    KeyCharacteristicsMetrics::kNumDocsTotalFieldName +
    "\", you may want to rerun the command at some other time to get more accurate \"" +
    KeyCharacteristicsMetrics::kNumDistinctValuesFieldName + "\" and \"" +
    KeyCharacteristicsMetrics::kMostCommonValuesFieldName + "\" metrics.";

/**
 * Validates that exactly one of 'sampleRate' and 'sampleSize' is specified.
 */
void validateSamplingOptions(boost::optional<double> sampleRate,
                             boost::optional<int64_t> sampleSize) {
    invariant(sampleRate || sampleSize, "Must specify one of 'sampleRate' and 'sampleSize'");
    invariant(!sampleRate || !sampleSize, "Cannot specify both 'sampleRate' and 'sampleSize'");
}

/**
 * Validates the metrics about the characteristics of a shard key.
 */
void validateKeyCharacteristicsMetrics(KeyCharacteristicsMetrics metrics) {
    const auto msg =
        "Unexpected error when calculating metrics about the cardinality and frequency of the "
        "shard key " +
        metrics.toBSON().toString();
    tassert(7826508, msg, metrics.getNumDocsTotal() >= metrics.getNumDocsSampled());
    if (metrics.getIsUnique()) {
        tassert(7826509, msg, metrics.getNumDocsSampled() == metrics.getNumDistinctValues());
    } else {
        tassert(7826510, msg, metrics.getNumDocsSampled() >= metrics.getNumDistinctValues());
    }
    tassert(
        7826511, msg, metrics.getNumDocsSampled() >= (int64_t)metrics.getMostCommonValues().size());
}

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
                                                                       int numMostCommonValues,
                                                                       int64_t numDocsTotal,
                                                                       int64_t numDocsToSample) {
    uassertStatusOK(validateIndexKey(hintIndexKey));

    std::vector<BSONObj> pipeline;

    pipeline.push_back(
        BSON("$project" << BSON("_id" << 0 << kIndexKeyFieldName << BSON("$meta" << "indexKey"))));

    if (numDocsTotal > numDocsToSample) {
        pipeline.push_back(
            BSON("$match" << BSON("$sampleRate" << (numDocsToSample * 1.0 / numDocsTotal))));
        pipeline.push_back(BSON("$limit" << numDocsToSample));
    }

    // Calculate the "frequency" of each original/hashed shard key value by doing a $group.
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

    // Calculate the "numDocs", "numDistinctValues" and "mostCommonValues" by doing a $group with
    // $topN.
    pipeline.push_back(BSON(
        "$group" << BSON(
            "_id" << BSONNULL << kNumDocsFieldName << BSON("$sum" << ("$" + kFrequencyFieldName))
                  << kNumDistinctValuesFieldName << BSON("$sum" << 1) << kMostCommonValuesFieldName
                  << BSON("$topN" << BSON("n" << numMostCommonValues << "sortBy"
                                              << BSON(kFrequencyFieldName << -1) << "output"
                                              << BSON("_id" << "$_id" << kFrequencyFieldName
                                                            << ("$" + kFrequencyFieldName)))))));

    // Unwind "mostCommonValues" to return each shard value in its own document.
    pipeline.push_back(BSON("$unwind" << ("$" + kMostCommonValuesFieldName)));

    // If the supporting index is hashed and the hashed field is one of the shard key fields, look
    // up the corresponding values by doing a $lookup with $toHashedIndexKey. Replace "_id"
    // with "doc" or "key" accordingly.
    if (origHashedFieldName) {
        invariant(tempHashedFieldName);

        pipeline.push_back(
            BSON("$set" << BSON(
                     "_id" << ("$" + kMostCommonValuesFieldName + "._id") << kFrequencyFieldName
                           << ("$" + kMostCommonValuesFieldName + "." + kFrequencyFieldName))));
        pipeline.push_back(BSON("$unset" << kMostCommonValuesFieldName));

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

        pipeline.push_back(
            BSON("$lookup" << BSON("from" << nss.coll() << "let" << letBuilder.obj() << "pipeline"
                                          << BSON_ARRAY(BSON("$match" << matchBuilder.obj())
                                                        << BSON("$limit" << 1))
                                          << "as"
                                          << "docs")));
        pipeline.push_back(BSON("$set" << BSON(kDocFieldName << BSON("$first" << "$docs"))));
        pipeline.push_back(BSON("$unset" << BSON_ARRAY("docs" << "_id")));
    } else {
        pipeline.push_back(BSON(
            "$set" << BSON(kIndexKeyFieldName
                           << ("$" + kMostCommonValuesFieldName + "._id") << kFrequencyFieldName
                           << ("$" + kMostCommonValuesFieldName + "." + kFrequencyFieldName))));
        pipeline.push_back(BSON("$unset" << BSON_ARRAY(kMostCommonValuesFieldName << "_id")));
    }

    AggregateCommandRequest aggRequest(nss, pipeline);
    aggRequest.setHint(hintIndexKey);
    aggRequest.setAllowDiskUse(true);
    // (re)shardCollection by design uses simple collation for comparing shard key values.
    aggRequest.setCollation(CollationSpec::kSimpleSpec);
    // Use readConcern "available" to avoid shard filtering since it is expensive.
    aggRequest.setReadConcern(repl::ReadConcernArgs::kAvailable);

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
        const auto collection = acquireCollectionMaybeLockFree(
            opCtx,
            CollectionAcquisitionRequest::fromOpCtx(opCtx, nss, AcquisitionPrerequisites::kRead));

        uassert(ErrorCodes::NamespaceNotFound,
                str::stream() << "Cannot run aggregation against a non-existing collection",
                collection.exists());

        collUuid = collection.uuid();
        collation = [&] {
            auto collationObj = aggRequest.getCollation();
            if (collationObj && !collationObj->isEmpty()) {
                return uassertStatusOK(CollatorFactoryInterface::get(opCtx->getServiceContext())
                                           ->makeFromBSON(*collationObj));
            }
            return CollatorInterface::cloneCollator(
                collection.getCollectionPtr()->getDefaultCollator());
        }();
    }

    if (MONGO_unlikely(analyzeShardKeyHangInClusterAggregate.shouldFail())) {
        analyzeShardKeyHangInClusterAggregate.pauseWhileSet();
    }

    ResolvedNamespaceMap resolvedNamespaces;
    resolvedNamespaces[nss] = {nss, std::vector<BSONObj>{}};

    auto pi = std::make_shared<ShardServerProcessInterface>(
        Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor());

    auto expCtx = ExpressionContextBuilder{}
                      .opCtx(opCtx)
                      .collator(std::move(collation))
                      .mongoProcessInterface(std::move(pi))
                      .ns(nss)
                      .resolvedNamespace(std::move(resolvedNamespaces))
                      .allowDiskUse(aggRequest.getAllowDiskUse())
                      .bypassDocumentValidation(true)
                      .collUUID(collUuid)
                      .tmpDir(boost::filesystem::path(storageGlobalParams.dbpath) / "_tmp")
                      .build();

    auto pipeline = Pipeline::makePipeline(aggRequest, expCtx);
    auto execPipeline = exec::agg::buildPipeline(pipeline->freeze());

    while (auto doc = execPipeline->getNext()) {
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
        collection->getIndexCatalog()->getIndexIterator(IndexCatalog::InclusionPolicy::kReady);
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
    int64_t numDocsTotal = 0;
    int64_t numDocsSampled = 0;
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
            if (element.type() == BSONType::object && depth == 0) {
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
                                                                   const UUID& analyzeShardKeyId,
                                                                   const NamespaceString& nss,
                                                                   const BSONObj& shardKey,
                                                                   int64_t numDocsTotal,
                                                                   int64_t numDocsToSample,
                                                                   int numMostCommonValues) {
    LOGV2(6915302,
          "Calculating cardinality and frequency for a unique shard key",
          logAttrs(nss),
          "analyzeShardKeyId"_attr = analyzeShardKeyId,
          "shardKey"_attr = shardKey,
          "numDocsTotal"_attr = numDocsTotal,
          "numDocsToSample"_attr = numDocsToSample,
          "numMostCommonValues"_attr = numMostCommonValues);

    CardinalityFrequencyMetrics metrics;

    numMostCommonValues = std::min(numMostCommonValues, (int)numDocsToSample);
    const auto maxSizeBytesPerValue = kMaxBSONObjSizeMostCommonValues / numMostCommonValues;

    std::vector<BSONObj> pipeline;
    pipeline.push_back(BSON("$match" << BSONObj()));
    pipeline.push_back(BSON("$limit" << numMostCommonValues));
    AggregateCommandRequest aggRequest(nss, pipeline);
    aggRequest.setReadConcern(extractReadConcern(opCtx));

    runAggregate(opCtx, aggRequest, [&](const BSONObj& doc) {
        auto value = bson::extractElementsBasedOnTemplate(doc.getOwned(), shardKey);
        if (value.objsize() > maxSizeBytesPerValue) {
            value = truncateBSONObj(value, maxSizeBytesPerValue);
        }
        metrics.mostCommonValues.emplace_back(std::move(value), 1);
    });

    uassert(7826506,
            "Cannot analyze the cardinality and frequency of a shard key for an empty collection",
            metrics.mostCommonValues.size() > 0);

    metrics.numDistinctValues = numDocsToSample;
    metrics.numDocsSampled = numDocsToSample;
    metrics.numDocsTotal = numDocsTotal;
    return metrics;
}

/**
 * Returns the cardinality and frequency metrics for the given shard key. Calculates the metrics by
 * running aggregation against the collection. If the shard key is unique, please use the version
 * above since the metrics can be determined without running any aggregations.
 */
CardinalityFrequencyMetrics calculateCardinalityAndFrequencyGeneric(OperationContext* opCtx,
                                                                    const UUID& analyzeShardKeyId,
                                                                    const NamespaceString& nss,
                                                                    const BSONObj& shardKey,
                                                                    const BSONObj& hintIndexKey,
                                                                    int64_t numDocsTotal,
                                                                    int64_t numDocsToSample,
                                                                    int numMostCommonValues) {
    LOGV2(6915303,
          "Calculating cardinality and frequency for a non-unique shard key",
          logAttrs(nss),
          "analyzeShardKeyId"_attr = analyzeShardKeyId,
          "shardKey"_attr = shardKey,
          "indexKey"_attr = hintIndexKey,
          "numDocsTotal"_attr = numDocsTotal,
          "numDocsToSample"_attr = numDocsToSample,
          "numMostCommonValues"_attr = numMostCommonValues);

    CardinalityFrequencyMetrics metrics;

    const auto maxSizeBytesPerValue = kMaxBSONObjSizeMostCommonValues / numMostCommonValues;

    auto aggRequest = makeAggregateRequestForCardinalityAndFrequency(
        nss, shardKey, hintIndexKey, numMostCommonValues, numDocsTotal, numDocsToSample);

    runAggregate(opCtx, aggRequest, [&](const BSONObj& doc) {
        auto numDocs = doc.getField(kNumDocsFieldName).exactNumberLong();
        invariant(numDocs > 0);
        if (metrics.numDocsSampled == 0) {
            metrics.numDocsSampled = numDocs;
        } else {
            invariant(metrics.numDocsSampled == numDocs);
        }

        auto numDistinctValues = doc.getField(kNumDistinctValuesFieldName).exactNumberLong();
        invariant(numDistinctValues > 0);
        if (metrics.numDistinctValues == 0) {
            metrics.numDistinctValues = numDistinctValues;
        } else {
            invariant(metrics.numDistinctValues == numDistinctValues);
        }

        auto value = [&] {
            if (doc.hasField(kIndexKeyFieldName)) {
                return bson::extractElementsBasedOnTemplate(
                    doc.getObjectField(kIndexKeyFieldName).replaceFieldNames(shardKey), shardKey);
            }
            if (doc.hasField(kDocFieldName)) {
                return bson::extractElementsBasedOnTemplate(doc.getObjectField(kDocFieldName),
                                                            shardKey);
            }
            uasserted(7588600,
                      str::stream() << "Failed to look up documents for most common shard key "
                                       "values in the command with \"analyzeShardKeyId\" "
                                    << analyzeShardKeyId
                                    << ". This is likely caused by concurrent deletions. "
                                       "Please try running the analyzeShardKey command again. "
                                    << redact(doc));
        }();
        if (value.objsize() > maxSizeBytesPerValue) {
            value = truncateBSONObj(value, maxSizeBytesPerValue);
        }
        auto frequency = doc.getField(kFrequencyFieldName).exactNumberLong();
        invariant(frequency > 0);
        metrics.mostCommonValues.emplace_back(std::move(value), frequency);
    });

    uassert(7826507,
            "Cannot analyze the cardinality and frequency of a shard key because the number of "
            "sampled documents is zero",
            metrics.numDocsSampled > 0);

    metrics.numDocsTotal = std::max(numDocsTotal, metrics.numDocsSampled);
    return metrics;
}

/**
 * Returns the cardinality and frequency metrics for a shard key given that the shard key is unique
 * and the collection has the the given fast count of the number of documents.
 */
CardinalityFrequencyMetrics calculateCardinalityAndFrequency(OperationContext* opCtx,
                                                             const UUID& analyzeShardKeyId,
                                                             const NamespaceString& nss,
                                                             const BSONObj& shardKey,
                                                             const BSONObj& hintIndexKey,
                                                             bool isUnique,
                                                             int64_t numDocsTotal,
                                                             boost::optional<double> sampleRate,
                                                             boost::optional<int64_t> sampleSize) {
    validateSamplingOptions(sampleRate, sampleSize);
    uassert(ErrorCodes::IllegalOperation,
            "Cannot analyze the cardinality and frequency of a shard key for an empty collection",
            numDocsTotal > 0);

    const auto numMostCommonValues = gNumMostCommonValues.load();
    uassert(ErrorCodes::InvalidOptions,
            str::stream() << "The requested number of most common values is " << numMostCommonValues
                          << " but the requested number of documents according to 'sampleSize'"
                          << " is " << sampleSize,
            !sampleSize || (*sampleSize >= numMostCommonValues));

    const auto numDocsToSample =
        sampleRate ? std::ceil(*sampleRate * numDocsTotal) : std::min(*sampleSize, numDocsTotal);
    return isUnique ? calculateCardinalityAndFrequencyUnique(opCtx,
                                                             analyzeShardKeyId,
                                                             nss,
                                                             shardKey,
                                                             numDocsTotal,
                                                             numDocsToSample,
                                                             numMostCommonValues)
                    : calculateCardinalityAndFrequencyGeneric(opCtx,
                                                              analyzeShardKeyId,
                                                              nss,
                                                              shardKey,
                                                              hintIndexKey,
                                                              numDocsTotal,
                                                              numDocsToSample,
                                                              numMostCommonValues);
}

/**
 * Returns the monotonicity metrics for the given shard key, i.e. whether the value of the given
 * shard key is monotonically changing in insertion order and the RecordId correlation coefficient
 * calculated by the monotonicity check. If the collection is clustered or the shard key does not
 * have a supporting index, returns 'unknown' and none.
 */
MonotonicityMetrics calculateMonotonicity(OperationContext* opCtx,
                                          const UUID& analyzeShardKeyId,
                                          const CollectionAcquisition& collection,
                                          const BSONObj& shardKey,
                                          boost::optional<double> sampleRate,
                                          boost::optional<int64_t> sampleSize) {
    validateSamplingOptions(sampleRate, sampleSize);

    LOGV2(6915304,
          "Calculating monotonicity",
          logAttrs(collection.nss()),
          "analyzeShardKeyId"_attr = analyzeShardKeyId,
          "shardKey"_attr = shardKey,
          "sampleRate"_attr = sampleRate,
          "sampleSize"_attr = sampleSize);

    MonotonicityMetrics metrics;

    const auto& collectionPtr = collection.getCollectionPtr();
    if (collectionPtr->isClustered()) {
        metrics.setType(MonotonicityTypeEnum::kUnknown);
        return metrics;
    }

    if (KeyPattern::isHashedKeyPattern(shardKey)) {
        if (shardKey.nFields() == 1 || shardKey.firstElement().valueStringDataSafe() == "hashed") {
            metrics.setType(MonotonicityTypeEnum::kNotMonotonic);
            metrics.setRecordIdCorrelationCoefficient(0);
        } else {
            // The monotonicity cannot be inferred from the recordIds in the index since hashing
            // introduces randomness.
            metrics.setType(MonotonicityTypeEnum::kUnknown);
        }
        return metrics;
    }

    const auto index = findShardKeyPrefixedIndex(opCtx,
                                                 collectionPtr,
                                                 shardKey,
                                                 /*requireSingleKey=*/true);

    if (!index) {
        metrics.setType(MonotonicityTypeEnum::kUnknown);
        return metrics;
    }
    // Non-clustered indexes always have an associated IndexDescriptor.
    invariant(index->descriptor());

    std::vector<int64_t> recordIds;
    bool scannedMultipleShardKeys = false;
    BSONObj firstShardKey;

    const int64_t numRecordsTotal = collectionPtr->numRecords(opCtx);
    uassert(serverGlobalParams.clusterRole.has(ClusterRole::ShardServer)
                ? ErrorCodes::CollectionIsEmptyLocally
                : ErrorCodes::IllegalOperation,
            "Cannot analyze the monotonicity of a shard key for an empty collection",
            numRecordsTotal > 0);

    const auto numRecordsToSample = sampleRate ? std::ceil(*sampleRate * numRecordsTotal)
                                               : std::min(*sampleSize, numRecordsTotal);
    const auto recordSampleRate =
        sampleRate ? *sampleRate : (numRecordsToSample * 1.0 / numRecordsTotal);

    LOGV2(7826504,
          "Start scanning the supporting index to get record ids",
          logAttrs(collection.nss()),
          "analyzeShardKeyId"_attr = analyzeShardKeyId,
          "shardKey"_attr = shardKey,
          "indexKey"_attr = index->keyPattern(),
          "numRecordsTotal"_attr = numRecordsTotal,
          "recordSampleRate"_attr = recordSampleRate,
          "numRecordsToSample"_attr = numRecordsToSample);

    KeyPattern indexKeyPattern(index->keyPattern());
    auto exec = InternalPlanner::indexScan(opCtx,
                                           collection,
                                           index->descriptor(),
                                           indexKeyPattern.globalMin(),
                                           indexKeyPattern.globalMax(),
                                           BoundInclusion::kExcludeBothStartAndEndKeys,
                                           PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY);
    auto prng = opCtx->getClient()->getPrng();

    try {
        RecordId recordId;
        BSONObj recordVal;
        while (recordIds.size() < numRecordsToSample) {
            auto shouldSample =
                (recordSampleRate == 1) || (prng.nextCanonicalDouble() < recordSampleRate);
            auto execState = shouldSample
                ? exec->getNext(scannedMultipleShardKeys ? nullptr : &recordVal, &recordId)
                : exec->getNext(nullptr, nullptr);

            if (execState != PlanExecutor::ADVANCED) {
                break;
            }
            if (!shouldSample) {
                continue;
            }

            recordIds.push_back(recordId.getLong());
            if (!scannedMultipleShardKeys) {
                auto currentShardKey = bson::extractElementsBasedOnTemplate(
                    recordVal.replaceFieldNames(shardKey), shardKey);
                if (recordIds.size() == 1) {
                    firstShardKey = currentShardKey;
                } else if (SimpleBSONObjComparator::kInstance.evaluate(firstShardKey !=
                                                                       currentShardKey)) {
                    scannedMultipleShardKeys = true;
                }
            }
        }
    } catch (DBException& ex) {
        LOGV2_WARNING(6875301, "Internal error while reading", "ns"_attr = collection.nss());
        ex.addContext("Executor error while reading during 'analyzeShardKey' command");
        throw;
    }

    uassert(7826505,
            "Cannot analyze the monotonicity because the number of sampled records is zero",
            recordIds.size() > 0);

    LOGV2(779009,
          "Finished scanning the supporting index. Start calculating correlation coefficient for "
          "the record ids in the supporting index",
          logAttrs(collection.nss()),
          "analyzeShardKeyId"_attr = analyzeShardKeyId,
          "shardKey"_attr = shardKey,
          "indexKey"_attr = indexKeyPattern,
          "numRecordIds"_attr = recordIds.size());

    if (!scannedMultipleShardKeys) {
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
          "Finished calculating correlation coefficient for the record ids in the supporting index",
          logAttrs(collection.nss()),
          "analyzeShardKeyId"_attr = analyzeShardKeyId,
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
    analyzeShardKeyPauseBeforeCalculatingCollStatsMetrics.pauseWhileSet(opCtx);

    CollStatsMetrics metrics;

    std::vector<BSONObj> pipeline;
    pipeline.push_back(BSON("$collStats" << BSON("storageStats" << BSONObj())));
    pipeline.push_back(BSON(
        "$group" << BSON("_id" << BSONNULL << kNumBytesFieldName
                               << BSON("$sum" << "$storageStats.size") << kNumDocsFieldName
                               << BSON("$sum" << "$storageStats.count") << kNumOrphanDocsFieldName
                               << BSON("$sum" << "$storageStats.numOrphanDocs"))));
    AggregateCommandRequest aggRequest(nss, pipeline);
    aggRequest.setReadConcern(extractReadConcern(opCtx));

    auto isShardedCollection = [&] {
        if (serverGlobalParams.clusterRole.has(ClusterRole::ShardServer)) {
            const auto cri = uassertStatusOK(
                Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfo(opCtx, nss));
            return cri.isSharded();
        }
        return false;
    }();

    runAggregate(opCtx, aggRequest, [&](const BSONObj& doc) {
        metrics.numDocs = doc.getField(kNumDocsFieldName).exactNumberLong();
        uassert(7826501,
                str::stream() << "The number of documents returned by $collStats indicates "
                                 "that the collection is empty. This is likely caused by an "
                                 "unclean shutdown that resulted in an inaccurate fast count or "
                                 "by deletions that have occurred since the command started. "
                              << doc,
                metrics.numDocs > 0);

        metrics.avgDocSizeBytes =
            doc.getField(kNumBytesFieldName).exactNumberLong() / metrics.numDocs;
        uassert(7826502,
                str::stream() << "The average document size calculated from metrics returned "
                                 "by $collStats is zero. This is likely caused by an unclean "
                                 "shutdown that resulted in an inaccurate fast count or by "
                                 "deletions that have occurred since the command started. "
                              << doc,
                metrics.avgDocSizeBytes > 0);

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
                                                  const UUID& analyzeShardKeyId,
                                                  const NamespaceString& nss,
                                                  const UUID& collUuid,
                                                  const KeyPattern& shardKey) {
    auto origCollUuid = getCollectionUUID(opCtx, nss);
    uassert(ErrorCodes::NamespaceNotFound,
            str::stream() << "Cannot analyze a shard key for a non-existing collection",
            origCollUuid);
    // Perform best-effort validation that the collection has not been dropped and recreated.
    uassert(
        CollectionUUIDMismatchInfo(nss.dbName(), collUuid, std::string{nss.coll()}, boost::none),
        str::stream() << "Found that the collection UUID has changed from " << collUuid << " to "
                      << origCollUuid << " since the command started",
        origCollUuid == collUuid);

    LOGV2(7559400,
          "Generating split points using the shard key being analyzed",
          logAttrs(nss),
          "analyzeShardKeyId"_attr = analyzeShardKeyId,
          "shardKey"_attr = shardKey);

    auto shardKeyPattern = ShardKeyPattern(shardKey);
    auto initialSplitter = SamplingBasedSplitPolicy::make(opCtx,
                                                          nss,
                                                          shardKeyPattern,
                                                          gNumShardKeyRanges.load(),
                                                          boost::none /*zones*/,
                                                          boost::none /*availableShardIds*/,
                                                          gNumSamplesPerShardKeyRange.load());
    auto splitPoints = [&] {
        try {
            return initialSplitter.createFirstSplitPoints(opCtx, shardKeyPattern);
        } catch (DBException& ex) {
            ex.addContext(str::stream()
                          << "Failed to find split points that partition the data into "
                          << gNumShardKeyRanges.load()
                          << " chunks with roughly equal number of documents using the shard key "
                             "being analyzed");
            throw;
        }
    }();
    uassert(10828001, "Expected to find at least one split point", !splitPoints.empty());

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

    auto expireAt = opCtx->fastClockSource().now() +
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
        doc.setId({analyzeShardKeyId, UUID::gen() /* splitPointId */});
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

    tassert(10828002,
            "Expected to have set the afterClusterTime since there is at least one split point "
            "document to insert",
            !splitPointsAfterClusterTime.isNull());
    auto splitPointsFilter = BSON((AnalyzeShardKeySplitPointDocument::kIdFieldName + "." +
                                   AnalyzeShardKeySplitPointId::kAnalyzeShardKeyIdFieldName)
                                  << analyzeShardKeyId);
    return {std::move(splitPointsFilter), splitPointsAfterClusterTime};
}

}  // namespace

boost::optional<KeyCharacteristicsMetrics> calculateKeyCharacteristicsMetrics(
    OperationContext* opCtx,
    const UUID& analyzeShardKeyId,
    const NamespaceString& nss,
    const UUID& collUuid,
    const KeyPattern& shardKey,
    boost::optional<double> sampleRate,
    boost::optional<int64_t> sampleSize) {
    invariant(!sampleRate || !sampleSize, "Cannot specify both 'sampleRate' and 'sampleSize'");
    // If both 'sampleRate' and 'sampleSize' are not specified, set 'sampleSize' to the default.
    if (!sampleRate && !sampleSize) {
        sampleSize = gKeyCharacteristicsDefaultSampleSize.load();
    }

    KeyCharacteristicsMetrics metrics;

    auto shardKeyBson = shardKey.toBSON();
    BSONObj indexKeyBson;
    {
        const auto collection = acquireCollectionMaybeLockFree(
            opCtx,
            CollectionAcquisitionRequest::fromOpCtx(opCtx, nss, AcquisitionPrerequisites::kRead));

        uassert(ErrorCodes::NamespaceNotFound,
                str::stream() << "Cannot analyze a shard key for a non-existing collection",
                collection.exists());
        // Perform best-effort validation that the collection has not been dropped and recreated.
        uassert(CollectionUUIDMismatchInfo(
                    nss.dbName(), collUuid, std::string{nss.coll()}, boost::none),
                str::stream() << "Found that the collection UUID has changed from " << collUuid
                              << " to " << collection.uuid() << " since the command started",
                collection.uuid() == collUuid);

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
        auto value = bson::extractElementsBasedOnTemplate(doc, shardKeyBson);
        uassertShardKeyValueNotContainArrays(value);

        // Restore the original readConcern.
        repl::ReadConcernArgs::get(opCtx) = originalReadConcernArgs;

        const auto& collectionPtr = collection.getCollectionPtr();
        auto indexSpec = findCompatiblePrefixedIndex(opCtx, collectionPtr, shardKeyBson);

        if (!indexSpec) {
            return boost::none;
        }

        indexKeyBson = indexSpec->keyPattern.getOwned();

        LOGV2(6915305,
              "Start calculating metrics about the characteristics of the shard key",
              logAttrs(nss),
              "analyzeShardKeyId"_attr = analyzeShardKeyId,
              "shardKey"_attr = shardKeyBson,
              "indexKey"_attr = indexKeyBson,
              "sampleRate"_attr = sampleRate,
              "sampleSize"_attr = sampleSize);
        analyzeShardKeyPauseBeforeCalculatingKeyCharacteristicsMetrics.pauseWhileSet(opCtx);

        metrics.setIsUnique(shardKeyBson.nFields() == indexKeyBson.nFields() ? indexSpec->isUnique
                                                                             : false);
        LOGV2(7790001,
              "Start calculating metrics about the monotonicity of the shard key",
              logAttrs(nss),
              "analyzeShardKeyId"_attr = analyzeShardKeyId);
        auto monotonicityMetrics = calculateMonotonicity(
            opCtx, analyzeShardKeyId, collection, shardKeyBson, sampleRate, sampleSize);
        LOGV2(7790002,
              "Finished calculating metrics about the monotonicity of the shard key",
              logAttrs(nss),
              "analyzeShardKeyId"_attr = analyzeShardKeyId);

        metrics.setMonotonicity(monotonicityMetrics);
    }

    LOGV2(7790003,
          "Start calculating metrics about the collection",
          logAttrs(nss),
          "analyzeShardKeyId"_attr = analyzeShardKeyId);
    auto collStatsMetrics = calculateCollStats(opCtx, nss);
    LOGV2(7790004,
          "Finished calculating metrics about the collection",
          logAttrs(nss),
          "analyzeShardKeyId"_attr = analyzeShardKeyId);

    LOGV2(7790005,
          "Start calculating metrics about the cardinality and frequency of the shard key",
          logAttrs(nss),
          "analyzeShardKeyId"_attr = analyzeShardKeyId);
    auto cardinalityFrequencyMetrics = calculateCardinalityAndFrequency(opCtx,
                                                                        analyzeShardKeyId,
                                                                        nss,
                                                                        shardKeyBson,
                                                                        indexKeyBson,
                                                                        metrics.getIsUnique(),
                                                                        collStatsMetrics.numDocs,
                                                                        sampleRate,
                                                                        sampleSize);
    LOGV2(7790006,
          "Finished calculating metrics about the cardinality and frequency of the shard key",
          logAttrs(nss),
          "analyzeShardKeyId"_attr = analyzeShardKeyId);

    // Use a tassert here since the IllegalOperation error should have been thrown while
    // calculating about the cardinality and frequency of the shard key.
    tassert(ErrorCodes::IllegalOperation,
            "Cannot analyze the characteristics of a shard key for an empty collection",
            cardinalityFrequencyMetrics.numDocsSampled > 0);

    metrics.setNumDocsTotal(cardinalityFrequencyMetrics.numDocsTotal);
    if (collStatsMetrics.numOrphanDocs) {
        metrics.setNumOrphanDocs(collStatsMetrics.numOrphanDocs);
        metrics.setNote(kOrphanDocsWarningMessage);
    }
    metrics.setAvgDocSizeBytes(collStatsMetrics.avgDocSizeBytes);

    metrics.setNumDocsSampled(cardinalityFrequencyMetrics.numDocsSampled);
    metrics.setNumDistinctValues(cardinalityFrequencyMetrics.numDistinctValues);
    metrics.setMostCommonValues(cardinalityFrequencyMetrics.mostCommonValues);
    validateKeyCharacteristicsMetrics(metrics);

    LOGV2(7790007,
          "Finished calculating metrics about the characteristics of the shard key",
          logAttrs(nss),
          "analyzeShardKeyId"_attr = analyzeShardKeyId,
          "shardKey"_attr = shardKeyBson,
          "indexKey"_attr = indexKeyBson);

    return metrics;
}

std::pair<ReadDistributionMetrics, WriteDistributionMetrics> calculateReadWriteDistributionMetrics(
    OperationContext* opCtx,
    const UUID& analyzeShardKeyId,
    const NamespaceString& nss,
    const UUID& collUuid,
    const KeyPattern& shardKey) {
    LOGV2(6915306,
          "Start calculating metrics about the read and write distribution",
          logAttrs(nss),
          "analyzeShardKeyId"_attr = analyzeShardKeyId,
          "shardKey"_attr = shardKey);
    analyzeShardKeyPauseBeforeCalculatingReadWriteDistributionMetrics.pauseWhileSet(opCtx);

    ReadDistributionMetrics readDistributionMetrics;
    WriteDistributionMetrics writeDistributionMetrics;

    auto [splitPointsFilter, splitPointsAfterClusterTime] =
        generateSplitPoints(opCtx, analyzeShardKeyId, nss, collUuid, shardKey);

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
            doc, IDLParserContext("calculateReadWriteDistributionMetrics"));
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

    LOGV2(7790008,
          "Finished calculating metrics about the read and write distribution",
          logAttrs(nss),
          "analyzeShardKeyId"_attr = analyzeShardKeyId,
          "shardKey"_attr = shardKey);

    return std::make_pair(readDistributionMetrics, writeDistributionMetrics);
}

}  // namespace analyze_shard_key
}  // namespace mongo
