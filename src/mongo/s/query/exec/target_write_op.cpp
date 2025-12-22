/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/s/query/exec/target_write_op.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/global_catalog/chunk_manager.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/collation/collation_index_key.h"
#include "mongo/db/query/collation/collation_spec.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/write_ops/update_request.h"
#include "mongo/db/query/write_ops/write_ops.h"
#include "mongo/db/server_options.h"
#include "mongo/db/shard_role/shard_catalog/raw_data_operation.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/timeseries/metadata.h"
#include "mongo/db/timeseries/timeseries_constants.h"
#include "mongo/db/timeseries/timeseries_options.h"
#include "mongo/db/timeseries/timeseries_update_delete_util.h"
#include "mongo/db/versioning_protocol/shard_version.h"
#include "mongo/logv2/log.h"
#include "mongo/s/query/shard_key_pattern_query_util.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <memory>
#include <utility>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {

namespace {
MONGO_FAIL_POINT_DEFINE(isTrackedTimeSeriesBucketsNamespaceAlwaysTrue);
MONGO_FAIL_POINT_DEFINE(isTrackedTimeSeriesNamespaceAlwaysTrue);

constexpr auto kIdFieldName = "_id"_sd;

const ShardKeyPattern kVirtualIdShardKey(BSON(kIdFieldName << 1));

using UpdateType = write_ops::UpdateModification::Type;

/**
 * Update expressions are bucketed into one of two types for the purposes of shard targeting:
 *
 * Replacement style: coll.update({ x : 1 }, { y : 2 })
 * OpStyle: coll.update({ x : 1 }, { $set : { y : 2 } })
 *            or
 *          coll.update({x: 1}, [{$addFields: {y: 2}}])
 */
void validateUpdateDoc(const UpdateOpRef& updateRef) {
    const auto& updateMod = updateRef.getUpdateMods();
    if (updateMod.type() == write_ops::UpdateModification::Type::kPipeline) {
        return;
    }

    // Non-pipeline style update.
    if (MONGO_unlikely(updateRef.getConstants())) {
        // Using 'c' field (constants) for a non-pipeline update is disallowed.
        UpdateRequest::throwUnexpectedConstantValuesException();
    }

    const auto updateType = updateMod.type();
    tassert(11428700,
            "Expected replacement update or modifier update",
            updateType == UpdateType::kReplacement || updateType == UpdateType::kModifier);

    const auto& updateExpr = updateType == UpdateType::kReplacement
        ? updateMod.getUpdateReplacement()
        : updateMod.getUpdateModifier();

    // Make sure that the update expression does not mix $op and non-$op fields.
    for (const auto& curField : updateExpr) {
        const auto updateTypeFromField = curField.fieldNameStringData()[0] != '$'
            ? UpdateType::kReplacement
            : UpdateType::kModifier;

        uassert(ErrorCodes::UnsupportedFormat,
                str::stream() << "update document " << updateExpr
                              << " has mixed $operator and non-$operator style fields",
                updateType == updateTypeFromField);
    }

    uassert(ErrorCodes::InvalidOptions,
            "Replacement-style updates cannot be {multi:true}",
            updateType == UpdateType::kModifier || !updateRef.getMulti());
}

ShardEndpoint targetUnshardedCollection(const NamespaceString& nss,
                                        const CollectionRoutingInfo& cri) {
    tassert(11428701, "Expected collection to be unsharded", !cri.isSharded());

    if (cri.hasRoutingTable()) {
        // Target the only shard that owns this collection.
        const auto shardId = cri.getChunkManager().getMinKeyShardIdWithSimpleCollation();
        return ShardEndpoint(shardId, cri.getShardVersion(shardId), boost::none);
    } else {
        // Target the db-primary shard. Attach 'dbVersion: X, shardVersion: UNTRACKED'.
        // TODO (SERVER-51070): Remove the boost::none when the config server can support
        // shardVersion in commands
        return ShardEndpoint(
            cri.getDbPrimaryShardId(),
            nss.isOnInternalDb() ? boost::optional<ShardVersion>() : ShardVersion::UNTRACKED(),
            nss.isOnInternalDb() ? boost::optional<DatabaseVersion>() : cri.getDbVersion());
    }
}
}  // namespace

BSONObj extractBucketsShardKeyFromTimeseriesDoc(const BSONObj& doc,
                                                const ShardKeyPattern& pattern,
                                                const TimeseriesOptions& timeseriesOptions) {
    allocator_aware::BSONObjBuilder builder;

    auto timeField = timeseriesOptions.getTimeField();
    auto timeElement = doc.getField(timeField);
    uassert(5743702,
            fmt::format("Timeseries time field '{}' must be present and contain a valid BSON UTC "
                        "datetime value",
                        timeField),
            !timeElement.eoo() && timeElement.type() == BSONType::date);
    auto roundedTimeValue =
        timeseries::roundTimestampToGranularity(timeElement.date(), timeseriesOptions);
    {
        allocator_aware::BSONObjBuilder controlBuilder{
            builder.subobjStart(timeseries::kBucketControlFieldName)};
        {
            allocator_aware::BSONObjBuilder minBuilder{
                controlBuilder.subobjStart(timeseries::kBucketControlMinFieldName)};
            minBuilder.append(timeField, roundedTimeValue);
        }
    }

    if (auto metaField = timeseriesOptions.getMetaField(); metaField) {
        if (auto metaElement = doc.getField(*metaField); !metaElement.eoo()) {
            timeseries::metadata::normalize(metaElement, builder, timeseries::kBucketMetaFieldName);
        }
    }

    auto docWithShardKey = builder.done();
    return pattern.extractShardKeyFromDoc(docWithShardKey);
}

namespace {
bool isExactIdQuery(const CanonicalQuery& query,
                    bool isSharded,
                    const CollatorInterface* defaultCollator) {
    auto shardKey = extractShardKeyFromQuery(kVirtualIdShardKey, query);
    BSONElement idElt = shardKey["_id"];

    if (!idElt) {
        return false;
    }

    if (CollationIndexKey::isCollatableType(idElt.type()) && isSharded &&
        !query.getFindCommandRequest().getCollation().isEmpty() &&
        !CollatorInterface::collatorsMatch(query.getCollator(), defaultCollator)) {

        // The collation applies to the _id field, but the user specified a collation which doesn't
        // match the collection default.
        return false;
    }

    return true;
}
}  // namespace

bool isExactIdQuery(OperationContext* opCtx,
                    const NamespaceString& nss,
                    const BSONObj& query,
                    const BSONObj& collation,
                    bool isSharded,
                    const CollatorInterface* defaultCollator) {
    auto findCommand = std::make_unique<FindCommandRequest>(nss);
    findCommand->setFilter(query);
    if (!collation.isEmpty()) {
        findCommand->setCollation(collation);
    }

    auto cq = CanonicalQuery::make({
        .expCtx = ExpressionContextBuilder{}.fromRequest(opCtx, *findCommand).build(),
        .parsedFind = ParsedFindCommandParams{.findCommand = std::move(findCommand),
                                              .allowedFeatures =
                                                  MatchExpressionParser::kAllowAllSpecialFeatures},
    });
    return cq.isOK() && isExactIdQuery(*cq.getValue(), isSharded, defaultCollator);
}

bool isTimeseriesLogicalOperation(OperationContext* opCtx,
                                  const CollectionRoutingInfo& cri,
                                  bool isViewfulTimeseries) {
    // For viewless timeseries collections logical operations are those that do not specifcy
    // rawData flag.
    //
    // For legacy viewful timeseries collection logical operations are the ones that either:
    //  - Target the view namespace and specify the rawData flag
    //  - Target directly the bucket namespace
    const auto& cm = cri.getChunkManager();
    return (cm.isTimeseriesCollection() && !isRawDataOperation(opCtx) &&
            (cm.isNewTimeseriesWithoutView() || isViewfulTimeseries));
}

ShardEndpoint targetInsert(OperationContext* opCtx,
                           const NamespaceString& nss,
                           const CollectionRoutingInfo& cri,
                           bool isViewfulTimeseries,
                           const BSONObj& doc) {
    if (!cri.isSharded()) {
        return targetUnshardedCollection(nss, cri);
    }

    // Collection is sharded
    const auto& cm = cri.getChunkManager();

    const BSONObj shardKey = [&]() {
        const auto& shardKeyPattern = cm.getShardKeyPattern();
        BSONObj shardKey;
        if (shardKeyPattern.hasId()) {
            uassert(ErrorCodes::InvalidIdField,
                    "Document is missing _id field, which is part of the shard key pattern",
                    doc.hasField("_id"));
        }
        if (isTimeseriesLogicalOperation(opCtx, cri, isViewfulTimeseries)) {
            const auto& tsFields = cm.getTimeseriesFields();
            tassert(5743701, "Missing timeseriesFields on timeseries collection", tsFields);
            shardKey = extractBucketsShardKeyFromTimeseriesDoc(
                doc, shardKeyPattern, tsFields->getTimeseriesOptions());
        } else {
            shardKey = shardKeyPattern.extractShardKeyFromDoc(doc);
        }

        // The shard key would only be empty after extraction if we encountered an error case, such
        // as the shard key possessing an array value or array descendants. If the shard key
        // presented to the targeter was empty, we would emplace the missing fields, and the
        // extracted key here would *not* be empty.
        uassert(ErrorCodes::ShardKeyNotFound,
                "Shard key cannot contain array values or array descendants.",
                !shardKey.isEmpty());
        return shardKey;
    }();

    // Target the shard key
    return uassertStatusOK(targetShardKey(cri, shardKey, CollationSpec::kSimpleSpec));
}

namespace {
bool isRetryableWrite(OperationContext* opCtx) {
    return opCtx->getTxnNumber() && !opCtx->inMultiDocumentTransaction();
}

boost::intrusive_ptr<ExpressionContext> makeExpressionContextWithDefaultsForTargeter(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const CollectionRoutingInfo& cri,
    const BSONObj& collation,
    const boost::optional<ExplainOptions::Verbosity>& verbosity,
    const boost::optional<BSONObj>& letParameters,
    const boost::optional<LegacyRuntimeConstants>& runtimeConstants) {

    const auto noCollationSpecified = collation.isEmpty();
    auto&& cif = [&]() {
        if (noCollationSpecified) {
            return std::unique_ptr<CollatorInterface>{};
        } else {
            return uassertStatusOK(
                CollatorFactoryInterface::get(opCtx->getServiceContext())->makeFromBSON(collation));
        }
    }();

    ResolvedNamespaceMap resolvedNamespaces;
    resolvedNamespaces.emplace(nss, ResolvedNamespace(nss, std::vector<BSONObj>{}));

    auto expCtx = ExpressionContextBuilder{}
                      .opCtx(opCtx)
                      .collator(std::move(cif))
                      .mongoProcessInterface(MongoProcessInterface::create(opCtx))
                      .ns(nss)
                      .resolvedNamespace(std::move(resolvedNamespaces))
                      .fromRouter(true)
                      .bypassDocumentValidation(true)
                      .explain(verbosity)
                      .runtimeConstants(runtimeConstants)
                      .letParameters(letParameters)
                      .build();

    // Ignore the collator if the collection is untracked and the user did not specify a collator.
    if (!cri.hasRoutingTable() && noCollationSpecified) {
        expCtx->setIgnoreCollator();
    }
    return expCtx;
}
}  // namespace

TargetOpResult targetUpdate(OperationContext* opCtx,
                            const NamespaceString& nss,
                            const CollectionRoutingInfo& cri,
                            bool isViewfulTimeseries,
                            const WriteOpRef& itemRef) {
    TargetOpResult result;
    auto updateOp = itemRef.getUpdateOp();
    const bool isMulti = updateOp.getMulti();

    const bool isFindAndModify = updateOp.getCommand().isFindAndModifyCommand();
    auto& updateOneUnshardedCount = isFindAndModify
        ? getQueryCounters(opCtx).findAndModifyUnshardedCount
        : getQueryCounters(opCtx).updateOneUnshardedCount;
    auto& updateOneTargetedShardedCount = isFindAndModify
        ? getQueryCounters(opCtx).findAndModifyTargetedShardedCount
        : getQueryCounters(opCtx).updateOneTargetedShardedCount;
    auto& updateOneNonTargetedShardedCount = isFindAndModify
        ? getQueryCounters(opCtx).findAndModifyNonTargetedShardedCount
        : getQueryCounters(opCtx).updateOneNonTargetedShardedCount;

    if (!cri.isSharded()) {
        if (isMulti) {
            getQueryCounters(opCtx).updateManyCount.increment(1);
        } else {
            updateOneUnshardedCount.increment(1);
        }

        result.endpoints.emplace_back(targetUnshardedCollection(nss, cri));
        return result;
    }

    // Collection is sharded
    const auto collation = write_ops::collationOf(updateOp);
    const bool isTimeseriesLogicalOp =
        isTimeseriesLogicalOperation(opCtx, cri, isViewfulTimeseries);

    auto expCtx = makeExpressionContextWithDefaultsForTargeter(
        opCtx,
        nss,
        cri,
        collation,
        boost::none,  // explain
        itemRef.getCommand().getLet(),
        itemRef.getCommand().getLegacyRuntimeConstants());

    const bool isUpsert = updateOp.getUpsert();
    auto query = updateOp.getFilter();
    const auto& cm = cri.getChunkManager();

    if (isTimeseriesLogicalOp) {
        uassert(ErrorCodes::InvalidOptions,
                str::stream()
                    << "A {multi:false} update on a sharded timeseries collection is disallowed.",
                feature_flags::gTimeseriesUpdatesSupport.isEnabled(
                    VersionContext::getDecoration(opCtx),
                    serverGlobalParams.featureCompatibility.acquireFCVSnapshot()) ||
                    isMulti);
        uassert(ErrorCodes::InvalidOptions,
                str::stream()
                    << "An {upsert:true} update on a sharded timeseries collection is disallowed.",
                feature_flags::gTimeseriesUpdatesSupport.isEnabled(
                    VersionContext::getDecoration(opCtx),
                    serverGlobalParams.featureCompatibility.acquireFCVSnapshot()) ||
                    !isUpsert);

        // Translate the update query on a timeseries collection into the bucket-level predicate
        // so that we can target the request to the correct shard or broadcast the request if
        // the bucket-level predicate is empty.
        //
        // Note: The query returned would match a super set of the documents matched by the
        // original query.
        query = timeseries::getBucketLevelPredicateForRouting(
            query,
            expCtx,
            cm.getTimeseriesFields()->getTimeseriesOptions(),
            feature_flags::gTimeseriesUpdatesSupport.isEnabled(
                VersionContext::getDecoration(opCtx),
                serverGlobalParams.featureCompatibility.acquireFCVSnapshot()));
    }

    validateUpdateDoc(updateOp);

    // Parse update query.
    const auto cq =
        uassertStatusOKWithContext(canonicalizeFindQuery(opCtx, expCtx, nss, query, collation, cri),
                                   str::stream() << "Could not parse update query " << query);

    // Target based on the update's filter.
    //
    // If this is a multi:true upsert, we call targetQueryForMultiUpsert() to do targeting.
    // For all other kinds of updates, we call targetQuery() to do targeting.
    //
    // In either case, if a non-OK status is returned we will throw an exception here.
    //
    // For now, a findAndModify request sets bypassIsFieldHashedCheck to be true in order to skip
    // the isFieldHashedCheck in the special case where _id is hashed and used as the shard key.
    // This means that we always assume that a findAndModify request using _id is targetable to a
    // single shard.
    const bool bypassIsFieldHashedCheck = isFindAndModify;
    result.endpoints = isMulti && isUpsert
        ? uassertStatusOK(targetQueryForMultiUpsert(cri, *cq))
        : uassertStatusOK(targetQuery(cri, *cq, bypassIsFieldHashedCheck));

    // For multi:true updates/upserts, there are no other checks to perform. Increment query
    // counters as appropriate and return 'result'.
    if (isMulti) {
        getQueryCounters(opCtx).updateManyCount.increment(1);
        return result;
    }

    bool multipleEndpoints = result.endpoints.size() > 1u;
    const bool isExactId =
        isExactIdQuery(*cq, cm.isSharded(), cm.getDefaultCollator()) && !isTimeseriesLogicalOp;

    // In the multiple shards scenario, for multi:false upserts, multi:false non-upsert updates
    // whose filter doesn't have an "_id" equality, and for findAndModify we use the two phase write
    // protocol.
    result.useTwoPhaseWriteProtocol =
        multipleEndpoints && (!isExactId || isUpsert || isFindAndModify);

    // For retryable multi:false non-upsert updates whose filter has an "_id" equality that involve
    // multiple shards (i.e. 'multipleEndpoints' is true), we execute by broadcasting the query to
    // all these shards (which is permissible because "_id" must be unique across all shards).
    // TODO SPM-3673: Implement a similar approach for non-retryable or sessionless multi:false
    // non-upsert updates with an "_id" equality that involve multiple shards.
    result.isNonTargetedRetryableWriteWithId =
        multipleEndpoints && isExactId && !isUpsert && !isFindAndModify && isRetryableWrite(opCtx);

    // Increment query counters as appropriate.
    if (!multipleEndpoints) {
        updateOneTargetedShardedCount.increment(1);
    } else {
        updateOneNonTargetedShardedCount.increment(1);

        if (isExactId) {
            getQueryCounters(opCtx).updateOneOpStyleBroadcastWithExactIDCount.increment(1);
        }

        if (isExactId && !isUpsert) {
            if (isRetryableWrite(opCtx)) {
                getQueryCounters(opCtx).updateOneWithoutShardKeyWithIdCount.increment(1);
            } else {
                getQueryCounters(opCtx).nonRetryableUpdateOneWithoutShardKeyWithIdCount.increment(
                    1);
            }
        }
    }

    return result;
}

TargetOpResult targetDelete(OperationContext* opCtx,
                            const NamespaceString& nss,
                            const CollectionRoutingInfo& cri,
                            bool isViewfulTimeseries,
                            const WriteOpRef& itemRef) {
    TargetOpResult result;

    auto deleteOp = itemRef.getDeleteOp();
    const bool isMulti = deleteOp.getMulti();
    const auto collation = write_ops::collationOf(deleteOp);

    const bool isFindAndModify = deleteOp.getCommand().isFindAndModifyCommand();
    auto& deleteOneUnshardedCount = isFindAndModify
        ? getQueryCounters(opCtx).findAndModifyUnshardedCount
        : getQueryCounters(opCtx).deleteOneUnshardedCount;
    auto& deleteOneTargetedShardedCount = isFindAndModify
        ? getQueryCounters(opCtx).findAndModifyTargetedShardedCount
        : getQueryCounters(opCtx).deleteOneTargetedShardedCount;
    auto& deleteOneNonTargetedShardedCount = isFindAndModify
        ? getQueryCounters(opCtx).findAndModifyNonTargetedShardedCount
        : getQueryCounters(opCtx).deleteOneNonTargetedShardedCount;

    if (!cri.isSharded()) {
        if (isMulti) {
            getQueryCounters(opCtx).deleteManyCount.increment(1);
        } else {
            deleteOneUnshardedCount.increment(1);
        }

        result.endpoints.emplace_back(targetUnshardedCollection(nss, cri));
        return result;
    }

    // Collection is sharded
    const bool isTimeseriesLogicalOp =
        isTimeseriesLogicalOperation(opCtx, cri, isViewfulTimeseries);

    auto expCtx = makeExpressionContextWithDefaultsForTargeter(
        opCtx,
        nss,
        cri,
        collation,
        boost::none,  // explain
        itemRef.getCommand().getLet(),
        itemRef.getCommand().getLegacyRuntimeConstants());

    BSONObj deleteQuery = deleteOp.getFilter();
    const auto& cm = cri.getChunkManager();

    if (isTimeseriesLogicalOp) {
        uassert(ErrorCodes::IllegalOperation,
                "Cannot perform a non-multi delete on a time-series collection",
                feature_flags::gTimeseriesDeletesSupport.isEnabled(
                    VersionContext::getDecoration(opCtx),
                    serverGlobalParams.featureCompatibility.acquireFCVSnapshot()) ||
                    isMulti);

        auto tsFields = cm.getTimeseriesFields();
        tassert(5918101, "Missing timeseriesFields on buckets collection", tsFields);

        // Translate the delete query on a timeseries collection into the bucket-level predicate
        // so that we can target the request to the correct shard or broadcast the request if
        // the bucket-level predicate is empty.
        //
        // Note: The query returned would match a super set of the documents matched by the
        // original query.
        deleteQuery = timeseries::getBucketLevelPredicateForRouting(
            deleteQuery,
            expCtx,
            tsFields->getTimeseriesOptions(),
            feature_flags::gTimeseriesDeletesSupport.isEnabled(
                VersionContext::getDecoration(opCtx),
                serverGlobalParams.featureCompatibility.acquireFCVSnapshot()));
    }

    // Parse delete query.
    const auto cq = uassertStatusOKWithContext(
        canonicalizeFindQuery(opCtx, expCtx, nss, deleteQuery, collation, cri),
        str::stream() << "Could not parse delete query " << deleteQuery);

    const bool isExactId =
        isExactIdQuery(*cq, cm.isSharded(), cm.getDefaultCollator()) && !isTimeseriesLogicalOp;

    // Target based on the delete's filter.
    //
    // For now, a findAndModify request sets bypassIsFieldHashedCheck to be true in order to skip
    // the isFieldHashedCheck in the special case where _id is hashed and used as the shard key.
    // This means that we always assume that a findAndModify request using _id is targetable to a
    // single shard.
    const bool bypassIsFieldHashedCheck = isFindAndModify;
    auto endpoints = uassertStatusOK(targetQuery(cri, *cq, bypassIsFieldHashedCheck));
    const bool multipleEndpoints = endpoints.size() > 1u;

    result.endpoints = std::move(endpoints);

    // For multi:true deletes, there are no other checks to perform. Increment query counters
    // as appropriate and return 'result'.
    if (isMulti) {
        getQueryCounters(opCtx).deleteManyCount.increment(1);
        return result;
    }

    result.useTwoPhaseWriteProtocol = multipleEndpoints && (!isExactId || isFindAndModify);

    result.isNonTargetedRetryableWriteWithId =
        isExactId && multipleEndpoints && isRetryableWrite(opCtx);

    // Increment query counters as appropriate.
    if (!multipleEndpoints) {
        deleteOneTargetedShardedCount.increment(1);
    } else {
        deleteOneNonTargetedShardedCount.increment(1);

        if (isExactId) {
            if (isRetryableWrite(opCtx)) {
                getQueryCounters(opCtx).deleteOneWithoutShardKeyWithIdCount.increment(1);
            } else {
                getQueryCounters(opCtx).nonRetryableDeleteOneWithoutShardKeyWithIdCount.increment(
                    1);
            }
        }
    }

    return result;
}

StatusWith<std::unique_ptr<CanonicalQuery>> canonicalizeFindQuery(
    OperationContext* opCtx,
    boost::intrusive_ptr<mongo::ExpressionContext> expCtx,
    const NamespaceString& nss,
    const BSONObj& query,
    const BSONObj& collation,
    const CollectionRoutingInfo& cri) {
    // Parse query.
    auto findCommand = std::make_unique<FindCommandRequest>(nss);
    findCommand->setFilter(query);

    const auto& cm = cri.getChunkManager();
    expCtx->setUUID(cm.getUUID());

    tassert(8557200,
            "This function should not be invoked if we do not have a routing table",
            cm.hasRoutingTable());
    if (!collation.isEmpty()) {
        findCommand->setCollation(collation.getOwned());
    } else if (cm.getDefaultCollator()) {
        auto defaultCollator = cm.getDefaultCollator();
        expCtx->setCollator(defaultCollator->clone());
    }

    return CanonicalQuery::make({
        .expCtx = expCtx,
        .parsedFind = ParsedFindCommandParams{.findCommand = std::move(findCommand),
                                              .allowedFeatures =
                                                  MatchExpressionParser::kAllowAllSpecialFeatures},
    });
}

StatusWith<std::vector<ShardEndpoint>> targetQuery(const CollectionRoutingInfo& cri,
                                                   const CanonicalQuery& query,
                                                   bool bypassIsFieldHashedCheck) {

    std::set<ShardId> shardIds;
    try {
        getShardIdsForCanonicalQuery(
            query, cri.getChunkManager(), &shardIds, bypassIsFieldHashedCheck);
    } catch (const DBException& ex) {
        return ex.toStatus();
    }

    std::vector<ShardEndpoint> endpoints;
    for (auto&& shardId : shardIds) {
        ShardVersion shardVersion = cri.getShardVersion(shardId);
        endpoints.emplace_back(std::move(shardId), std::move(shardVersion), boost::none);
    }

    return endpoints;
}

StatusWith<std::vector<ShardEndpoint>> targetQueryForMultiUpsert(const CollectionRoutingInfo& cri,
                                                                 const CanonicalQuery& query) {
    // Attempt to extract the shard key from the query.
    const auto& shardKeyPattern = cri.getChunkManager().getShardKeyPattern();
    BSONObj shardKey = extractShardKeyFromQuery(shardKeyPattern, query);

    // If extracting the shard key failed, return a non-OK Status.
    if (shardKey.isEmpty()) {
        return Status{ErrorCodes::ShardKeyNotFound,
                      "Failed to target upsert by query :: could not extract exact shard key"};
    }

    // Call targetShardKey() and throw an error if this fails.
    const BSONObj& collation = query.getFindCommandRequest().getCollation();
    auto swEndpoint = targetShardKey(cri, shardKey, collation);

    // If targetShardKey() returned a non-OK Status, return that status with added context.
    if (!swEndpoint.isOK()) {
        return swEndpoint.getStatus().withContext("Failed to target upsert by query");
    }

    // Store the result of targetShardKey() into a vector and return the vector.
    std::vector<ShardEndpoint> endpoints;
    endpoints.emplace_back(std::move(swEndpoint.getValue()));
    return endpoints;
}

StatusWith<ShardEndpoint> targetShardKey(const CollectionRoutingInfo& cri,
                                         const BSONObj& shardKey,
                                         const BSONObj& collation) {
    try {
        auto chunk = cri.getChunkManager().findIntersectingChunk(shardKey, collation);
        return ShardEndpoint(
            chunk.getShardId(), cri.getShardVersion(chunk.getShardId()), boost::none);
    } catch (const DBException& ex) {
        return ex.toStatus();
    }
}

std::vector<ShardEndpoint> targetAllShards(OperationContext* opCtx,
                                           const CollectionRoutingInfo& cri) {
    // This function is only called if doing a multi write that targets more than one shard. This
    // implies the collection is sharded, so we should always have a chunk manager.
    tassert(11428703, "Expected collection to be sharded", cri.isSharded());

    auto shardIds = Grid::get(opCtx)->shardRegistry()->getAllShardIds(opCtx);

    std::vector<ShardEndpoint> endpoints;
    for (auto&& shardId : shardIds) {
        ShardVersion shardVersion = cri.getShardVersion(shardId);
        endpoints.emplace_back(std::move(shardId), std::move(shardVersion), boost::none);
    }

    return endpoints;
}

int getAproxNShardsOwningChunks(const CollectionRoutingInfo& cri) {
    return cri.hasRoutingTable() ? cri.getChunkManager().getAproxNShardsOwningChunks() : 0;
}

bool isTrackedTimeSeriesBucketsNamespace(const CollectionRoutingInfo& cri) {
    // Used for testing purposes to force that we always have a tracked timeseries bucket namespace.
    if (MONGO_unlikely(isTrackedTimeSeriesBucketsNamespaceAlwaysTrue.shouldFail())) {
        return true;
    }
    return cri.hasRoutingTable() && cri.getChunkManager().isTimeseriesCollection() &&
        !cri.getChunkManager().isNewTimeseriesWithoutView();
}

bool isTrackedTimeSeriesNamespace(const CollectionRoutingInfo& cri) {
    // Used for testing purposes to force that we always have a tracked timeseries namespace.
    if (MONGO_unlikely(isTrackedTimeSeriesNamespaceAlwaysTrue.shouldFail())) {
        return true;
    }
    return cri.hasRoutingTable() && cri.getChunkManager().isTimeseriesCollection();
}

}  // namespace mongo
