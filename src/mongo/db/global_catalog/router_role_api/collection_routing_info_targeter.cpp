/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/global_catalog/router_role_api/collection_routing_info_targeter.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/global_catalog/chunk.h"
#include "mongo/db/global_catalog/ddl/cluster_ddl.h"
#include "mongo/db/global_catalog/router_role_api/cluster_commands_helpers.h"
#include "mongo/db/global_catalog/shard_key_pattern_query_util.h"
#include "mongo/db/global_catalog/type_collection_common_types_gen.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/collation/collation_index_key.h"
#include "mongo/db/query/collation/collation_spec.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/write_ops/update_request.h"
#include "mongo/db/query/write_ops/write_ops.h"
#include "mongo/db/query/write_ops/write_ops_parsers.h"
#include "mongo/db/raw_data_operation.h"
#include "mongo/db/server_options.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/sharding_feature_flags_gen.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/db/timeseries/metadata.h"
#include "mongo/db/timeseries/timeseries_constants.h"
#include "mongo/db/timeseries/timeseries_options.h"
#include "mongo/db/timeseries/timeseries_update_delete_util.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/db/version_context.h"
#include "mongo/db/versioning_protocol/shard_version.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/compiler.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"

#include <memory>
#include <string>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(waitForDatabaseToBeDropped);
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
    invariant(updateType == UpdateType::kReplacement || updateType == UpdateType::kModifier);
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

/**
 * Returns true if the two CollectionRoutingInfo objects are different.
 */
bool isMetadataDifferent(const CollectionRoutingInfo& criA, const CollectionRoutingInfo& criB) {
    if (criA.hasRoutingTable() != criB.hasRoutingTable())
        return true;

    if (criA.hasRoutingTable()) {
        return criA.getChunkManager().getVersion() != criB.getChunkManager().getVersion();
    }

    return criA.getDbVersion() != criB.getDbVersion();
}

ShardEndpoint targetUnshardedCollection(const NamespaceString& nss,
                                        const CollectionRoutingInfo& cri) {
    invariant(!cri.isSharded());
    if (cri.hasRoutingTable()) {
        // Target the only shard that owns this collection.
        const auto shardId = cri.getChunkManager().getMinKeyShardIdWithSimpleCollation();
        return ShardEndpoint(shardId, cri.getShardVersion(shardId), boost::none);
    } else {
        // Target the db-primary shard. Attach 'dbVersion: X, shardVersion: UNSHARDED'.
        // TODO (SERVER-51070): Remove the boost::none when the config server can support
        // shardVersion in commands
        return ShardEndpoint(
            cri.getDbPrimaryShardId(),
            nss.isOnInternalDb() ? boost::optional<ShardVersion>() : ShardVersion::UNSHARDED(),
            nss.isOnInternalDb() ? boost::optional<DatabaseVersion>() : cri.getDbVersion());
    }
}

}  // namespace

const size_t CollectionRoutingInfoTargeter::kMaxDatabaseCreationAttempts = 3;

CollectionRoutingInfoTargeter::CollectionRoutingInfoTargeter(OperationContext* opCtx,
                                                             const NamespaceString& nss,
                                                             boost::optional<OID> targetEpoch)
    : _nss(nss),
      _targetEpoch(std::move(targetEpoch)),
      _routingCtx(_init(opCtx, false)),
      _cri(_routingCtx->getCollectionRoutingInfo(_nss)) {}

CollectionRoutingInfoTargeter::CollectionRoutingInfoTargeter(const NamespaceString& nss,
                                                             const CollectionRoutingInfo& cri)
    : _nss(nss), _routingCtx(RoutingContext::createSynthetic({{nss, cri}})), _cri(cri) {
    invariant(!cri.hasRoutingTable() || cri.getChunkManager().getNss() == nss);
}

/**
 * Initializes and returns the RoutingContext which needs to be used for targeting.
 * If 'refresh' is true, additionally fetches the latest routing info from the config servers.
 *
 * Note: For tracked time-series collections, we use the buckets collection for targeting. If the
 * user request is on the view namespace, we implicitly transform the request to the buckets
 * namespace.
 */
std::unique_ptr<RoutingContext> CollectionRoutingInfoTargeter::_init(OperationContext* opCtx,
                                                                     bool refresh) {
    const auto createDatabaseAndGetRoutingCtx = [&opCtx, &refresh](const NamespaceString& nss) {
        size_t attempts = 1;
        while (true) {
            try {
                cluster::createDatabase(opCtx, nss.dbName());

                if (refresh) {
                    Grid::get(opCtx)->catalogCache()->onStaleCollectionVersion(
                        nss, boost::none /* wantedVersion */);
                }

                if (MONGO_unlikely(waitForDatabaseToBeDropped.shouldFail())) {
                    LOGV2(8314600, "Hanging due to waitForDatabaseToBeDropped fail point");
                    waitForDatabaseToBeDropped.pauseWhileSet(opCtx);
                }

                return uassertStatusOK(getRoutingContextForTxnCmd(opCtx, {nss}));
            } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
                LOGV2_INFO(8314601,
                           "Failed initialization of routing info because the database has been "
                           "concurrently dropped",
                           logAttrs(nss.dbName()),
                           "attemptNumber"_attr = attempts,
                           "maxAttempts"_attr = kMaxDatabaseCreationAttempts);

                if (attempts++ >= kMaxDatabaseCreationAttempts) {
                    // The maximum number of attempts has been reached, so the procedure fails as it
                    // could be a logical error. At this point, it is unlikely that the error is
                    // caused by concurrent drop database operations.
                    throw;
                }
            }
        }
    };

    uassert(ErrorCodes::InvalidNamespace,
            str::stream() << "Must use a real namespace with CollectionRoutingInfoTargeter, got "
                          << _nss.toStringForErrorMsg(),
            !_nss.isCollectionlessAggregateNS());
    auto routingCtx = createDatabaseAndGetRoutingCtx(_nss);
    const auto& cri = routingCtx->getCollectionRoutingInfo(_nss);
    auto cm = std::move(cri.getChunkManager());

    const auto checkStaleEpoch = [&](ChunkManager& cm) {
        if (_targetEpoch) {
            uassert(StaleEpochInfo(_nss, ShardVersion{}, ShardVersion{}),
                    "Collection has been dropped",
                    cm.hasRoutingTable());
            uassert(StaleEpochInfo(_nss, ShardVersion{}, ShardVersion{}),
                    "Collection epoch has changed",
                    cm.getVersion().epoch() == *_targetEpoch);
        }
    };

    // For a tracked viewful time-series collection, only the underlying buckets collection is
    // stored on the config servers. If the user operation is on the time-series view namespace, we
    // should check if the buckets namespace is tracked on the configsvr. There are a few cases that
    // we need to take care of:
    // 1. The request is on the view namespace. We check if the buckets collection is tracked. If it
    //    is, we use the buckets collection namespace for the purpose of targeting. Additionally, we
    //    set the `_nssConvertedToTimeseriesBuckets` to true for this case.
    // 2. If request is on the buckets namespace, we don't need to execute any additional
    //    time-series logic. We can treat the request as though it was a request on a regular
    //    collection.
    // 3. During a cache refresh the buckets collection changes from tracked to untracked. In this
    //    case, if the original request is on the view namespace, then we should reset the namespace
    //    back to the view namespace and reset `_nssConvertedToTimeseriesBuckets`.
    //
    // TODO SERVER-106874 remove this if/else block entirely once 9.0 becomes last LTS. By then we
    // will only have viewless timeseries that do not require nss translation.
    if (!cm.hasRoutingTable() && !_nss.isTimeseriesBucketsCollection()) {
        auto bucketsNs = _nss.makeTimeseriesBucketsNamespace();
        auto bucketsRoutingCtx = createDatabaseAndGetRoutingCtx(bucketsNs);
        const auto& bucketsCri = bucketsRoutingCtx->getCollectionRoutingInfo(bucketsNs);
        if (bucketsCri.hasRoutingTable()) {
            _nss = bucketsNs;
            cm = std::move(bucketsCri.getChunkManager());
            _nssConvertedToTimeseriesBuckets = true;
            checkStaleEpoch(cm);
            return bucketsRoutingCtx;
        }
    } else if (!cm.hasRoutingTable() && _nssConvertedToTimeseriesBuckets) {
        // This can happen if a tracked time-series collection is dropped and re-created. Then we
        // need to reset the namespace to the original namespace.
        _nss = _nss.getTimeseriesViewNamespace();
        auto newRoutingCtx = createDatabaseAndGetRoutingCtx(_nss);
        cm = std::move(newRoutingCtx->getCollectionRoutingInfo(_nss).getChunkManager());
        _nssConvertedToTimeseriesBuckets = false;
        checkStaleEpoch(cm);
        return newRoutingCtx;
    }

    checkStaleEpoch(cm);
    return routingCtx;
}

const NamespaceString& CollectionRoutingInfoTargeter::getNS() const {
    return _nss;
}

BSONObj CollectionRoutingInfoTargeter::extractBucketsShardKeyFromTimeseriesDoc(
    const BSONObj& doc,
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

bool CollectionRoutingInfoTargeter::_isExactIdQuery(const CanonicalQuery& query,
                                                    const ChunkManager& cm) {
    auto shardKey = extractShardKeyFromQuery(kVirtualIdShardKey, query);
    BSONElement idElt = shardKey["_id"];

    if (!idElt) {
        return false;
    }

    if (CollationIndexKey::isCollatableType(idElt.type()) && cm.isSharded() &&
        !query.getFindCommandRequest().getCollation().isEmpty() &&
        !CollatorInterface::collatorsMatch(query.getCollator(), cm.getDefaultCollator())) {

        // The collation applies to the _id field, but the user specified a collation which doesn't
        // match the collection default.
        return false;
    }

    return true;
}

bool CollectionRoutingInfoTargeter::isExactIdQuery(OperationContext* opCtx,
                                                   const NamespaceString& nss,
                                                   const BSONObj& query,
                                                   const BSONObj& collation,
                                                   const ChunkManager& cm) {
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
    return cq.isOK() && _isExactIdQuery(*cq.getValue(), cm);
}

bool CollectionRoutingInfoTargeter::_isTimeseriesLogicalOperation(OperationContext* opCtx) const {
    // For viewless timeseries collections logical operations are those that do not specifcy
    // rawData flag.
    //
    // For legacy viewful timeseries collection logical operations are the ones that either:
    //  - Target the view namespace and specify the rawData flag
    //  - Target directly the bucket namespace
    return _cri.getChunkManager().isTimeseriesCollection() && !isRawDataOperation(opCtx) &&
        (_cri.getChunkManager().isNewTimeseriesWithoutView() || _nssConvertedToTimeseriesBuckets);
}

ShardEndpoint CollectionRoutingInfoTargeter::targetInsert(OperationContext* opCtx,
                                                          const BSONObj& doc) const {
    if (!_cri.isSharded()) {
        return targetUnshardedCollection(_nss, _cri);
    }

    // Collection is sharded
    const BSONObj shardKey = [&]() {
        const auto& shardKeyPattern = _cri.getChunkManager().getShardKeyPattern();
        BSONObj shardKey;
        if (shardKeyPattern.hasId()) {
            uassert(ErrorCodes::InvalidIdField,
                    "Document is missing _id field, which is part of the shard key pattern",
                    doc.hasField("_id"));
        }
        if (_isTimeseriesLogicalOperation(opCtx)) {
            const auto& tsFields = _cri.getChunkManager().getTimeseriesFields();
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
    return uassertStatusOK(_targetShardKey(shardKey, CollationSpec::kSimpleSpec));
}

bool isRetryableWrite(OperationContext* opCtx) {
    return opCtx->getTxnNumber() && !opCtx->inMultiDocumentTransaction();
}

NSTargeter::TargetingResult CollectionRoutingInfoTargeter::targetUpdate(
    OperationContext* opCtx, const BatchItemRef& itemRef) const {
    NSTargeter::TargetingResult result;
    auto updateOp = itemRef.getUpdateOp();
    const bool isMulti = updateOp.getMulti();

    if (!_cri.isSharded()) {
        if (isMulti) {
            getQueryCounters(opCtx).updateManyCount.increment(1);
        } else {
            getQueryCounters(opCtx).updateOneUnshardedCount.increment(1);
        }

        result.endpoints.emplace_back(targetUnshardedCollection(_nss, _cri));
        return result;
    }

    // Collection is sharded
    const auto collation = write_ops::collationOf(updateOp);

    auto expCtx = makeExpressionContextWithDefaultsForTargeter(
        opCtx,
        _nss,
        _cri,
        collation,
        boost::none,  // explain
        itemRef.getCommand().getLet(),
        itemRef.getCommand().getLegacyRuntimeConstants());

    const bool isUpsert = updateOp.getUpsert();
    auto query = updateOp.getFilter();

    if (_isTimeseriesLogicalOperation(opCtx)) {
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
            _cri.getChunkManager().getTimeseriesFields()->getTimeseriesOptions(),
            feature_flags::gTimeseriesUpdatesSupport.isEnabled(
                VersionContext::getDecoration(opCtx),
                serverGlobalParams.featureCompatibility.acquireFCVSnapshot()));
    }

    validateUpdateDoc(updateOp);

    // Parse update query.
    const auto cq = uassertStatusOKWithContext(
        _canonicalize(opCtx, expCtx, _nss, query, collation, _cri.getChunkManager()),
        str::stream() << "Could not parse update query " << query);

    // Target based on the update's filter.
    //
    // If this is a multi:true upsert, we call _targetQueryForMultiUpsert() to do targeting.
    // For all other kinds of updates, we call _targetQuery() to do targeting.
    //
    // In either case, if a non-OK status is returned we will throw an exception here.
    result.endpoints = isMulti && isUpsert ? uassertStatusOK(_targetQueryForMultiUpsert(*cq))
                                           : uassertStatusOK(_targetQuery(*cq));

    // For multi:true updates/upserts, there are no other checks to perform. Increment query
    // counters as appropriate and return 'result'.
    if (isMulti) {
        getQueryCounters(opCtx).updateManyCount.increment(1);
        return result;
    }

    bool multipleEndpoints = result.endpoints.size() > 1u;
    const bool isExactId =
        _isExactIdQuery(*cq, _cri.getChunkManager()) && !_isTimeseriesLogicalOperation(opCtx);

    // For multi:false upserts that involve multiple shards, and for multi:false non-upsert updates
    // whose filter doesn't have an "_id" equality that involve multiple shards, we use the two
    // phase write protocol.
    result.useTwoPhaseWriteProtocol = multipleEndpoints && (!isExactId || isUpsert);

    // For retryable multi:false non-upsert updates whose filter has an "_id" equality that involve
    // multiple shards (i.e. 'multipleEndpoints' is true), we execute by broadcasting the query to
    // all these shards (which is permissible because "_id" must be unique across all shards).
    // TODO SPM-3673: Implement a similar approach for non-retryable or sessionless multi:false
    // non-upsert updates with an "_id" equality that involve multiple shards.
    result.isNonTargetedRetryableWriteWithId =
        multipleEndpoints && isExactId && !isUpsert && isRetryableWrite(opCtx);

    // Increment query counters as appropriate.
    if (!multipleEndpoints) {
        getQueryCounters(opCtx).updateOneTargetedShardedCount.increment(1);
    } else {
        getQueryCounters(opCtx).updateOneNonTargetedShardedCount.increment(1);

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


NSTargeter::TargetingResult CollectionRoutingInfoTargeter::targetDelete(
    OperationContext* opCtx, const BatchItemRef& itemRef) const {
    NSTargeter::TargetingResult result;

    auto deleteOp = itemRef.getDeleteOp();
    const bool isMulti = deleteOp.getMulti();
    const auto collation = write_ops::collationOf(deleteOp);

    if (!_cri.isSharded()) {
        if (isMulti) {
            getQueryCounters(opCtx).deleteManyCount.increment(1);
        } else {
            getQueryCounters(opCtx).deleteOneUnshardedCount.increment(1);
        }

        result.endpoints.emplace_back(targetUnshardedCollection(_nss, _cri));
        return result;
    }

    // Collection is sharded
    auto expCtx = makeExpressionContextWithDefaultsForTargeter(
        opCtx,
        _nss,
        _cri,
        collation,
        boost::none,  // explain
        itemRef.getCommand().getLet(),
        itemRef.getCommand().getLegacyRuntimeConstants());

    BSONObj deleteQuery = deleteOp.getFilter();

    if (_isTimeseriesLogicalOperation(opCtx)) {
        uassert(ErrorCodes::IllegalOperation,
                "Cannot perform a non-multi delete on a time-series collection",
                feature_flags::gTimeseriesDeletesSupport.isEnabled(
                    VersionContext::getDecoration(opCtx),
                    serverGlobalParams.featureCompatibility.acquireFCVSnapshot()) ||
                    isMulti);

        auto tsFields = _cri.getChunkManager().getTimeseriesFields();
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
        _canonicalize(opCtx, expCtx, _nss, deleteQuery, collation, _cri.getChunkManager()),
        str::stream() << "Could not parse delete query " << deleteQuery);

    const bool isExactId =
        _isExactIdQuery(*cq, _cri.getChunkManager()) && !_isTimeseriesLogicalOperation(opCtx);

    // Target based on the delete's filter.
    auto endpoints = uassertStatusOK(_targetQuery(*cq));
    const bool multipleEndpoints = endpoints.size() > 1u;

    result.endpoints = std::move(endpoints);

    // For multi:true deletes, there are no other checks to perform. Increment query counters
    // as appropriate and return 'result'.
    if (isMulti) {
        getQueryCounters(opCtx).deleteManyCount.increment(1);
        return result;
    }

    result.useTwoPhaseWriteProtocol = !isExactId && multipleEndpoints;

    result.isNonTargetedRetryableWriteWithId =
        isExactId && multipleEndpoints && isRetryableWrite(opCtx);

    // Increment query counters as appropriate.
    if (!multipleEndpoints) {
        getQueryCounters(opCtx).deleteOneTargetedShardedCount.increment(1);
    } else {
        getQueryCounters(opCtx).deleteOneNonTargetedShardedCount.increment(1);

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

StatusWith<std::unique_ptr<CanonicalQuery>> CollectionRoutingInfoTargeter::_canonicalize(
    OperationContext* opCtx,
    boost::intrusive_ptr<mongo::ExpressionContext> expCtx,
    const NamespaceString& nss,
    const BSONObj& query,
    const BSONObj& collation,
    const ChunkManager& cm) {

    // Parse query.
    auto findCommand = std::make_unique<FindCommandRequest>(nss);
    findCommand->setFilter(query);
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

StatusWith<std::vector<ShardEndpoint>> CollectionRoutingInfoTargeter::_targetQuery(
    const CanonicalQuery& query) const {

    std::set<ShardId> shardIds;
    try {
        getShardIdsForCanonicalQuery(query, _cri.getChunkManager(), &shardIds);
    } catch (const DBException& ex) {
        return ex.toStatus();
    }

    std::vector<ShardEndpoint> endpoints;
    for (auto&& shardId : shardIds) {
        ShardVersion shardVersion = _cri.getShardVersion(shardId);
        endpoints.emplace_back(std::move(shardId), std::move(shardVersion), boost::none);
    }

    return endpoints;
}

StatusWith<std::vector<ShardEndpoint>> CollectionRoutingInfoTargeter::_targetQueryForMultiUpsert(
    const CanonicalQuery& query) const {
    // Attempt to extract the shard key from the query.
    const auto& shardKeyPattern = _cri.getChunkManager().getShardKeyPattern();
    BSONObj shardKey = extractShardKeyFromQuery(shardKeyPattern, query);

    // If extracting the shard key failed, return a non-OK Status.
    if (shardKey.isEmpty()) {
        return Status{ErrorCodes::ShardKeyNotFound,
                      "Failed to target upsert by query :: could not extract exact shard key"};
    }

    // Call _targetShardKey() and throw an error if this fails.
    const BSONObj& collation = query.getFindCommandRequest().getCollation();
    auto swEndpoint = _targetShardKey(shardKey, collation);

    // If _targetShardKey() returned a non-OK Status, return that status with added context.
    if (!swEndpoint.isOK()) {
        return swEndpoint.getStatus().withContext("Failed to target upsert by query");
    }

    // Store the result of _targetShardKey() into a vector and return the vector.
    std::vector<ShardEndpoint> endpoints;
    endpoints.emplace_back(std::move(swEndpoint.getValue()));
    return endpoints;
}

StatusWith<ShardEndpoint> CollectionRoutingInfoTargeter::_targetShardKey(
    const BSONObj& shardKey, const BSONObj& collation) const {
    try {
        auto chunk = _cri.getChunkManager().findIntersectingChunk(shardKey, collation);
        return ShardEndpoint(
            chunk.getShardId(), _cri.getShardVersion(chunk.getShardId()), boost::none);
    } catch (const DBException& ex) {
        return ex.toStatus();
    }
}

std::vector<ShardEndpoint> CollectionRoutingInfoTargeter::targetAllShards(
    OperationContext* opCtx) const {
    // This function is only called if doing a multi write that targets more than one shard. This
    // implies the collection is sharded, so we should always have a chunk manager.
    invariant(_cri.isSharded());

    auto shardIds = Grid::get(opCtx)->shardRegistry()->getAllShardIds(opCtx);

    std::vector<ShardEndpoint> endpoints;
    for (auto&& shardId : shardIds) {
        ShardVersion shardVersion = _cri.getShardVersion(shardId);
        endpoints.emplace_back(std::move(shardId), std::move(shardVersion), boost::none);
    }

    return endpoints;
}

void CollectionRoutingInfoTargeter::noteCouldNotTarget() {
    dassert(!_lastError || _lastError.value() == LastErrorType::kCouldNotTarget);
    _lastError = LastErrorType::kCouldNotTarget;
}

void CollectionRoutingInfoTargeter::noteStaleCollVersionResponse(OperationContext* opCtx,
                                                                 const StaleConfigInfo& staleInfo) {
    dassert(!_lastError || _lastError.value() == LastErrorType::kStaleShardVersion);
    Grid::get(opCtx)->catalogCache()->onStaleCollectionVersion(staleInfo.getNss(),
                                                               staleInfo.getVersionWanted());

    if (staleInfo.getNss() != _nss) {
        // This can happen when a time-series collection becomes sharded.
        Grid::get(opCtx)->catalogCache()->onStaleCollectionVersion(_nss, boost::none);
    }

    _lastError = LastErrorType::kStaleShardVersion;
}

void CollectionRoutingInfoTargeter::noteStaleDbVersionResponse(
    OperationContext* opCtx, const StaleDbRoutingVersion& staleInfo) {
    dassert(!_lastError || _lastError.value() == LastErrorType::kStaleDbVersion);
    Grid::get(opCtx)->catalogCache()->onStaleDatabaseVersion(_nss.dbName(),
                                                             staleInfo.getVersionWanted());
    _lastError = LastErrorType::kStaleDbVersion;
}

bool CollectionRoutingInfoTargeter::hasStaleShardResponse() {
    return _lastError &&
        (_lastError.value() == LastErrorType::kStaleShardVersion ||
         _lastError.value() == LastErrorType::kStaleDbVersion);
}

void CollectionRoutingInfoTargeter::noteCannotImplicitlyCreateCollectionResponse(
    OperationContext* opCtx, const CannotImplicitlyCreateCollectionInfo& createInfo) {
    dassert(!_lastError || _lastError.value() == LastErrorType::kCannotImplicitlyCreateCollection);

    // TODO (SERVER-82939) Remove this check once the namespaces are guaranteed to match.
    //
    // In the case that a bulk write is performing operations on two different namespaces, a
    // CannotImplicitlyCreateCollection error for one namespace can be duplicated to operations on
    // the other namespace. In this case, we only need to create the collection for the namespace
    // the error actually refers to.
    if (createInfo.getNss() == getNS()) {
        _lastError = LastErrorType::kCannotImplicitlyCreateCollection;
    }
}

bool CollectionRoutingInfoTargeter::refreshIfNeeded(OperationContext* opCtx) {
    // Did we have any stale config or targeting errors at all?
    if (!_lastError) {
        return false;
    }

    // Make sure that even in case of exception we will clear the last error.
    ON_BLOCK_EXIT([&] { _lastError = boost::none; });

    LOGV2_DEBUG(22912,
                4,
                "CollectionRoutingInfoTargeter checking if refresh is needed",
                "couldNotTarget"_attr = _lastError.value() == LastErrorType::kCouldNotTarget,
                "staleShardVersion"_attr = _lastError.value() == LastErrorType::kStaleShardVersion,
                "staleDbVersion"_attr = _lastError.value() == LastErrorType::kStaleDbVersion);

    // Get the latest metadata information from the cache if there were issues
    auto lastManager = _cri;
    _routingCtx = _init(opCtx, false);
    _cri = _routingCtx->getCollectionRoutingInfo(_nss);
    auto metadataChanged = isMetadataDifferent(lastManager, _cri);

    if (_lastError.value() == LastErrorType::kCouldNotTarget && !metadataChanged) {
        // If we couldn't target, and we didn't already update the metadata we must force a refresh
        _routingCtx = _init(opCtx, true);
        _cri = _routingCtx->getCollectionRoutingInfo(_nss);
        metadataChanged = isMetadataDifferent(lastManager, _cri);
    }

    return metadataChanged;
}

bool CollectionRoutingInfoTargeter::createCollectionIfNeeded(OperationContext* opCtx) {
    if (!_lastError || _lastError != LastErrorType::kCannotImplicitlyCreateCollection) {
        return false;
    }

    try {
        cluster::createCollectionWithRouterLoop(opCtx, getNS());
        LOGV2_DEBUG(8037201, 3, "Successfully created collection", "nss"_attr = getNS());
    } catch (const DBException& ex) {
        LOGV2(8037200, "Could not create collection", "error"_attr = redact(ex.toStatus()));
        _lastError = boost::none;
        return false;
    }
    // Ensure the routing info is refreshed before the command is retried to avoid StaleConfig
    _lastError = LastErrorType::kStaleShardVersion;
    return true;
}

int CollectionRoutingInfoTargeter::getAproxNShardsOwningChunks() const {
    if (_cri.hasRoutingTable()) {
        return _cri.getChunkManager().getAproxNShardsOwningChunks();
    }

    return 0;
}

bool CollectionRoutingInfoTargeter::isTargetedCollectionSharded() const {
    return _cri.isSharded();
}

bool CollectionRoutingInfoTargeter::isTrackedTimeSeriesBucketsNamespace() const {
    // Used for testing purposes to force that we always have a tracked timeseries bucket namespace.
    if (MONGO_unlikely(isTrackedTimeSeriesBucketsNamespaceAlwaysTrue.shouldFail())) {
        return true;
    }
    return _cri.hasRoutingTable() && _cri.getChunkManager().isTimeseriesCollection() &&
        !_cri.getChunkManager().isNewTimeseriesWithoutView();
}

bool CollectionRoutingInfoTargeter::isTrackedTimeSeriesNamespace() const {
    // Used for testing purposes to force that we always have a tracked timeseries namespace.
    if (MONGO_unlikely(isTrackedTimeSeriesNamespaceAlwaysTrue.shouldFail())) {
        return true;
    }
    return _cri.hasRoutingTable() && _cri.getChunkManager().isTimeseriesCollection();
}

bool CollectionRoutingInfoTargeter::timeseriesNamespaceNeedsRewrite(
    const NamespaceString& nss) const {
    return isTrackedTimeSeriesBucketsNamespace() && !nss.isTimeseriesBucketsCollection();
}

RoutingContext& CollectionRoutingInfoTargeter::getRoutingCtx() const {
    return *_routingCtx;
}

const CollectionRoutingInfo& CollectionRoutingInfoTargeter::getRoutingInfo() const {
    return _cri;
}

}  // namespace mongo
