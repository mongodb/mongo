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

#include "mongo/s/collection_routing_info_targeter.h"

#include "mongo/s/transaction_router.h"
#include <fmt/format.h>
#include <memory>
#include <string>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/commands/server_status_metric.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/internal_transactions_feature_flag_gen.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/db/ops/write_ops_parsers.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/collation/collation_index_key.h"
#include "mongo/db/query/collation/collation_spec.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/server_options.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/db/timeseries/metadata.h"
#include "mongo/db/timeseries/timeseries_constants.h"
#include "mongo/db/timeseries/timeseries_options.h"
#include "mongo/db/timeseries/timeseries_update_delete_util.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/s/chunk.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/cluster_ddl.h"
#include "mongo/s/grid.h"
#include "mongo/s/index_version.h"
#include "mongo/s/shard_key_pattern_query_util.h"
#include "mongo/s/shard_version.h"
#include "mongo/s/sharding_feature_flags_gen.h"
#include "mongo/s/sharding_index_catalog_cache.h"
#include "mongo/s/type_collection_common_types_gen.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(waitForDatabaseToBeDropped);
MONGO_FAIL_POINT_DEFINE(isTrackedTimeSeriesBucketsNamespaceAlwaysTrue);

constexpr auto kIdFieldName = "_id"_sd;

const ShardKeyPattern kVirtualIdShardKey(BSON(kIdFieldName << 1));

using UpdateType = write_ops::UpdateModification::Type;
using shard_key_pattern_query_util::QueryTargetingInfo;

// Tracks the number of {multi:false} updates with an exact match on _id that are broadcasted to
// multiple shards.
auto& updateOneOpStyleBroadcastWithExactIDCount =
    *MetricBuilder<Counter64>("query.updateOneOpStyleBroadcastWithExactIDCount");

/**
 * Update expressions are bucketed into one of two types for the purposes of shard targeting:
 *
 * Replacement style: coll.update({ x : 1 }, { y : 2 })
 * OpStyle: coll.update({ x : 1 }, { $set : { y : 2 } })
 *            or
 *          coll.update({x: 1}, [{$addFields: {y: 2}}])
 */
void validateUpdateDoc(const UpdateRef updateRef) {
    const auto& updateMod = updateRef.getUpdateMods();
    if (updateMod.type() == write_ops::UpdateModification::Type::kPipeline) {
        return;
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
 * Obtain the update expression from the given update doc. If this is a replacement-style update,
 * and the shard key includes _id but the replacement document does not, we attempt to find an exact
 * _id match in the query component and add it to the doc. We do this because mongoD will propagate
 * _id from the existing document if this is an update, and will extract _id from the query when
 * generating the new document in the case of an upsert. It is therefore always correct to target
 * the operation on the basis of the combined updateExpr and query.
 */
BSONObj getUpdateExprForTargeting(const boost::intrusive_ptr<ExpressionContext> expCtx,
                                  const ShardKeyPattern& shardKeyPattern,
                                  const BSONObj& updateQuery,
                                  const write_ops::UpdateModification& updateMod) {
    // If this is not a replacement update, then the update expression remains unchanged.
    if (updateMod.type() != UpdateType::kReplacement) {
        BSONObjBuilder objBuilder;
        updateMod.serializeToBSON("u", &objBuilder);
        return objBuilder.obj();
    }

    // Extract the raw update expression from the request.
    invariant(updateMod.type() == UpdateType::kReplacement);

    // Replace any non-existent shard key values with a null value.
    auto updateExpr =
        shardKeyPattern.emplaceMissingShardKeyValuesForDocument(updateMod.getUpdateReplacement());

    // If we aren't missing _id, return the update expression as-is.
    if (updateExpr.hasField(kIdFieldName)) {
        return updateExpr;
    }

    // We are missing _id, so attempt to extract it from an exact match in the update's query spec.
    // This will guarantee that we can target a single shard, but it is not necessarily fatal if no
    // exact _id can be found.
    const auto idFromQuery = uassertStatusOK(
        extractShardKeyFromBasicQueryWithContext(expCtx, kVirtualIdShardKey, updateQuery));
    if (auto idElt = idFromQuery[kIdFieldName]) {
        updateExpr = updateExpr.addField(idElt);
    }

    return updateExpr;
}

/**
 * Returns true if the two CollectionRoutingInfo objects are different.
 */
bool isMetadataDifferent(const CollectionRoutingInfo& criA, const CollectionRoutingInfo& criB) {
    if (criA.cm.hasRoutingTable() != criB.cm.hasRoutingTable())
        return true;

    if (criA.cm.hasRoutingTable()) {
        if (criA.cm.getVersion() != criB.cm.getVersion())
            return true;

        if (criA.sii.is_initialized() != criB.sii.is_initialized())
            return true;

        return criA.sii.is_initialized() &&
            criA.sii->getCollectionIndexes() != criB.sii->getCollectionIndexes();
    }

    return criA.cm.dbVersion() != criB.cm.dbVersion();
}

ShardEndpoint targetUnshardedCollection(const NamespaceString& nss,
                                        const CollectionRoutingInfo& cri) {
    invariant(!cri.cm.isSharded());
    if (cri.cm.hasRoutingTable()) {
        // Target the only shard that owns this collection.
        std::set<ShardId> shardsOwningChunks;
        cri.cm.getAllShardIds(&shardsOwningChunks);
        invariant(shardsOwningChunks.size() == 1);
        return ShardEndpoint(*shardsOwningChunks.begin(),
                             cri.getShardVersion(*shardsOwningChunks.begin()),
                             boost::none);
    } else {
        // Target the db-primary shard. Attach 'dbVersion: X, shardVersion: UNSHARDED'.
        // TODO (SERVER-51070): Remove the boost::none when the config server can support
        // shardVersion in commands
        return ShardEndpoint(
            cri.cm.dbPrimary(),
            nss.isOnInternalDb() ? boost::optional<ShardVersion>() : ShardVersion::UNSHARDED(),
            nss.isOnInternalDb() ? boost::optional<DatabaseVersion>() : cri.cm.dbVersion());
    }
}

}  // namespace

const size_t CollectionRoutingInfoTargeter::kMaxDatabaseCreationAttempts = 3;

CollectionRoutingInfoTargeter::CollectionRoutingInfoTargeter(OperationContext* opCtx,
                                                             const NamespaceString& nss,
                                                             boost::optional<OID> targetEpoch)
    : _nss(nss), _targetEpoch(std::move(targetEpoch)), _cri(_init(opCtx, false)) {}

CollectionRoutingInfoTargeter::CollectionRoutingInfoTargeter(const NamespaceString& nss,
                                                             const CollectionRoutingInfo& cri)
    : _nss(nss), _cri(cri) {
    invariant(!cri.cm.hasRoutingTable() || cri.cm.getNss() == nss);
}

/**
 * Initializes and returns the CollectionRoutingInfo which needs to be used for targeting.
 * If 'refresh' is true, additionally fetches the latest routing info from the config servers.
 *
 * Note: For tracked time-series collections, we use the buckets collection for targeting. If the
 * user request is on the view namespace, we implicitly transform the request to the buckets
 * namespace.
 */
CollectionRoutingInfo CollectionRoutingInfoTargeter::_init(OperationContext* opCtx, bool refresh) {
    auto [cm, sii] = [&] {
        size_t attempts = 1;
        while (true) {
            try {
                cluster::createDatabase(opCtx, _nss.dbName());

                if (refresh) {
                    uassertStatusOK(
                        Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfoWithRefresh(
                            opCtx, _nss));
                }

                if (MONGO_unlikely(waitForDatabaseToBeDropped.shouldFail())) {
                    LOGV2(8314600, "Hanging due to waitForDatabaseToBeDropped fail point");
                    waitForDatabaseToBeDropped.pauseWhileSet(opCtx);
                }

                return uassertStatusOK(getCollectionRoutingInfoForTxnCmd(opCtx, _nss));
            } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
                LOGV2_INFO(8314601,
                           "Failed initialization of routing info because the database has been "
                           "concurrently dropped",
                           logAttrs(_nss.dbName()),
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
    }();

    // For a tracked time-series collection, only the underlying buckets collection is stored on the
    // config servers. If the user operation is on the time-series view namespace, we should check
    // if the buckets namespace is tracked on the configsvr. There are a few cases that we need to
    // take care of:
    // 1. The request is on the view namespace. We check if the buckets collection is tracked. If it
    //    is, we use the buckets collection namespace for the purpose of targeting. Additionally, we
    //    set the '_isRequestOnTimeseriesViewNamespace' to true for this case.
    // 2. If request is on the buckets namespace, we don't need to execute any additional
    //    time-series logic. We can treat the request as though it was a request on a regular
    //    collection.
    // 3. During a cache refresh the buckets collection changes from tracked to untracked. In this
    //    case, if the original request is on the view namespace, then we should reset the namespace
    //    back to the view namespace and reset '_isRequestOnTimeseriesViewNamespace'.
    if (!cm.hasRoutingTable() && !_nss.isTimeseriesBucketsCollection()) {
        auto bucketsNs = _nss.makeTimeseriesBucketsNamespace();
        if (refresh) {
            uassertStatusOK(Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfoWithRefresh(
                opCtx, bucketsNs));
        }
        auto [bucketsPlacementInfo, bucketsIndexInfo] =
            uassertStatusOK(getCollectionRoutingInfoForTxnCmd(opCtx, bucketsNs));
        if (bucketsPlacementInfo.hasRoutingTable()) {
            _nss = bucketsNs;
            cm = std::move(bucketsPlacementInfo);
            sii = std::move(bucketsIndexInfo);
            _isRequestOnTimeseriesViewNamespace = true;
        }
    } else if (!cm.hasRoutingTable() && _isRequestOnTimeseriesViewNamespace) {
        // This can happen if a tracked time-series collection is dropped and re-created. Then we
        // need to reset the namespace to the original namespace.
        _nss = _nss.getTimeseriesViewNamespace();

        if (refresh) {
            uassertStatusOK(
                Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfoWithRefresh(opCtx, _nss));
        }
        auto [newCm, newSii] = uassertStatusOK(getCollectionRoutingInfoForTxnCmd(opCtx, _nss));
        cm = std::move(newCm);
        sii = std::move(newSii);
        _isRequestOnTimeseriesViewNamespace = false;
    }

    if (_targetEpoch) {
        uassert(ErrorCodes::StaleEpoch, "Collection has been dropped", cm.hasRoutingTable());
        uassert(ErrorCodes::StaleEpoch,
                "Collection epoch has changed",
                cm.getVersion().epoch() == *_targetEpoch);
    }
    return CollectionRoutingInfo(std::move(cm), std::move(sii));
}

const NamespaceString& CollectionRoutingInfoTargeter::getNS() const {
    return _nss;
}

BSONObj CollectionRoutingInfoTargeter::extractBucketsShardKeyFromTimeseriesDoc(
    const BSONObj& doc,
    const ShardKeyPattern& pattern,
    const TimeseriesOptions& timeseriesOptions) {
    BSONObjBuilder builder;

    auto timeField = timeseriesOptions.getTimeField();
    auto timeElement = doc.getField(timeField);
    uassert(5743702,
            str::stream() << "'" << timeField
                          << "' must be present and contain a valid BSON UTC datetime value",
            !timeElement.eoo() && timeElement.type() == BSONType::Date);
    auto roundedTimeValue =
        timeseries::roundTimestampToGranularity(timeElement.date(), timeseriesOptions);
    {
        BSONObjBuilder controlBuilder{builder.subobjStart(timeseries::kBucketControlFieldName)};
        {
            BSONObjBuilder minBuilder{
                controlBuilder.subobjStart(timeseries::kBucketControlMinFieldName)};
            minBuilder.append(timeField, roundedTimeValue);
        }
    }

    if (auto metaField = timeseriesOptions.getMetaField(); metaField) {
        if (auto metaElement = doc.getField(*metaField); !metaElement.eoo()) {
            timeseries::metadata::normalize(metaElement, builder, timeseries::kBucketMetaFieldName);
        }
    }

    auto docWithShardKey = builder.obj();
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
        .expCtx = makeExpressionContext(opCtx, *findCommand),
        .parsedFind = ParsedFindCommandParams{.findCommand = std::move(findCommand),
                                              .allowedFeatures =
                                                  MatchExpressionParser::kAllowAllSpecialFeatures},
    });
    return cq.isOK() && _isExactIdQuery(*cq.getValue(), cm);
}

ShardEndpoint CollectionRoutingInfoTargeter::targetInsert(OperationContext* opCtx,
                                                          const BSONObj& doc,
                                                          std::set<ChunkRange>* chunkRanges) const {
    if (!_cri.cm.isSharded()) {
        return targetUnshardedCollection(_nss, _cri);
    }

    // Collection is sharded
    const BSONObj shardKey = [&]() {
        const auto& shardKeyPattern = _cri.cm.getShardKeyPattern();
        BSONObj shardKey;
        if (shardKeyPattern.hasId()) {
            uassert(ErrorCodes::InvalidIdField,
                    "Document is missing _id field, which is part of the shard key pattern",
                    doc.hasField("_id"));
        }
        if (_isRequestOnTimeseriesViewNamespace) {
            auto tsFields = _cri.cm.getTimeseriesFields();
            tassert(5743701, "Missing timeseriesFields on buckets collection", tsFields);
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
    return uassertStatusOK(_targetShardKey(shardKey, CollationSpec::kSimpleSpec, chunkRanges));
}

std::vector<ShardEndpoint> CollectionRoutingInfoTargeter::targetUpdate(
    OperationContext* opCtx,
    const BatchItemRef& itemRef,
    bool* useTwoPhaseWriteProtocol,
    bool* isNonTargetedWriteWithoutShardKeyWithExactId,
    std::set<ChunkRange>* chunkRanges) const {
    // If the update is replacement-style:
    // 1. Attempt to target using the query. If this fails, AND the query targets more than one
    //    shard,
    // 2. Fall back to targeting using the replacement document.
    //
    // If the update is an upsert:
    // 1. Always attempt to target using the query. Upserts must have the full shard key in the
    //    query.
    //
    // NOTE: A replacement document is allowed to have missing shard key values, because we target
    // as if the shard key values are specified as NULL. A replacement document is also allowed
    // to have a missing '_id', and if the '_id' exists in the query, it will be emplaced in the
    // replacement document for targeting purposes.

    const auto& updateOp = itemRef.getUpdateRef();
    const bool isMulti = updateOp.getMulti();

    if (isMulti) {
        updateManyCount.increment(1);
    }

    if (!_cri.cm.isSharded()) {
        if (!isMulti) {
            updateOneUnshardedCount.increment(1);
        }

        return {targetUnshardedCollection(_nss, _cri)};
    }

    // Collection is sharded
    const auto& shardKeyPattern = _cri.cm.getShardKeyPattern();
    const auto collation = write_ops::collationOf(updateOp);

    auto expCtx = makeExpressionContextWithDefaultsForTargeter(opCtx,
                                                               _nss,
                                                               _cri,
                                                               collation,
                                                               boost::none,  // explain
                                                               itemRef.getLet(),
                                                               itemRef.getLegacyRuntimeConstants());

    const bool isUpsert = updateOp.getUpsert();
    auto query = updateOp.getFilter();

    if (_isRequestOnTimeseriesViewNamespace) {
        uassert(ErrorCodes::InvalidOptions,
                str::stream()
                    << "A {multi:false} update on a sharded timeseries collection is disallowed.",
                feature_flags::gTimeseriesUpdatesSupport.isEnabled(
                    serverGlobalParams.featureCompatibility.acquireFCVSnapshot()) ||
                    isMulti);
        uassert(ErrorCodes::InvalidOptions,
                str::stream()
                    << "An {upsert:true} update on a sharded timeseries collection is disallowed.",
                feature_flags::gTimeseriesUpdatesSupport.isEnabled(
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
            _cri.cm.getTimeseriesFields()->getTimeseriesOptions(),
            feature_flags::gTimeseriesUpdatesSupport.isEnabled(
                serverGlobalParams.featureCompatibility.acquireFCVSnapshot()));
    }

    validateUpdateDoc(updateOp);
    const auto updateExpr =
        getUpdateExprForTargeting(expCtx, shardKeyPattern, query, updateOp.getUpdateMods());

    // Utility function to target an update by shard key, and to handle any potential error results.
    auto targetByShardKey = [this, &collation, &chunkRanges, isUpsert, isMulti](
                                StatusWith<BSONObj> swShardKey, std::string msg) {
        const auto& shardKey = uassertStatusOKWithContext(std::move(swShardKey), msg);
        if (shardKey.isEmpty()) {
            uasserted(ErrorCodes::ShardKeyNotFound,
                      str::stream() << msg << " :: could not extract exact shard key");
        } else {
            return std::vector{
                uassertStatusOKWithContext(_targetShardKey(shardKey, collation, chunkRanges), msg)};
        }
    };

    // Parse update query.
    const auto cq =
        uassertStatusOKWithContext(_canonicalize(opCtx, expCtx, _nss, query, collation, _cri.cm),
                                   str::stream() << "Could not parse update query " << query);

    // With the introduction of PM-1632, we can use the two phase write protocol to successfully
    // target an upsert without the full shard key. Else, the query must contain an exact match
    // on the shard key. If we were to target based on the replacement doc, it could result in an
    // insertion even if a document matching the query exists on another shard.
    if ((!feature_flags::gFeatureFlagUpdateOneWithoutShardKey
              .isEnabledUseLastLTSFCVWhenUninitialized(
                  serverGlobalParams.featureCompatibility.acquireFCVSnapshot()) ||
         updateOp.getMulti()) &&
        isUpsert) {
        return targetByShardKey(extractShardKeyFromQuery(shardKeyPattern, *cq),
                                "Failed to target upsert by query");
    }

    auto isExactId = _isExactIdQuery(*cq, _cri.cm) && !_isRequestOnTimeseriesViewNamespace;

    // We first try to target based on the update's query. It is always valid to forward any update
    // or upsert to a single shard, so return immediately if we are able to target a single shard.
    auto endPoints = uassertStatusOK(_targetQuery(*cq, collation, chunkRanges));
    if (endPoints.size() == 1) {
        // The check is structured in this way to check if the query does not contain a shard key,
        // but is still targetable to a single shard. We don't explicitly use the result of
        // targetByShardKey(), and instead only see if we throw in that helper in order to determine
        // if the two phase write protocol should be used (in the case of a query that does not have
        // a shard key).
        try {
            targetByShardKey(extractShardKeyFromQuery(shardKeyPattern, *cq),
                             "Query does not contain the shard key.");
        } catch (const DBException&) {
            if (useTwoPhaseWriteProtocol && !isExactId) {
                *useTwoPhaseWriteProtocol = true;
            }
        }
        updateOneTargetedShardedCount.increment(1);
        return endPoints;
    }

    // Targeting by replacement document is no longer necessary when an updateOne without shard key
    // is allowed, since we're able to decisively select a document to modify with the two phase
    // write without shard key protocol.
    if (!feature_flags::gFeatureFlagUpdateOneWithoutShardKey.isEnabled(
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot()) ||
        isExactId) {
        // Replacement-style updates must always target a single shard. If we were unable to do so
        // using the query, we attempt to extract the shard key from the replacement and target
        // based on it.
        if (updateOp.getUpdateMods().type() == write_ops::UpdateModification::Type::kReplacement) {
            if (chunkRanges) {
                chunkRanges->clear();
            }
            return targetByShardKey(shardKeyPattern.extractShardKeyFromDoc(updateExpr),
                                    "Failed to target update by replacement document");
        }
    }

    // If we are here then this is an op-style update, and we were not able to target a single
    // shard. Non-multi updates must target a single shard or an exact _id. Time-series single
    // updates must target a single shard.
    uassert(
        ErrorCodes::InvalidOptions,
        fmt::format("A {{multi:false}} update on a sharded {} contain the shard key (and have the "
                    "simple collation), but this update targeted {} shards. Update request: {}, "
                    "shard key pattern: {}",
                    _isRequestOnTimeseriesViewNamespace
                        ? "time-series collection must"
                        : "collection must contain an exact match on _id (and have the "
                          "collection default collation) or",
                    endPoints.size(),
                    updateOp.toBSON().toString(),
                    shardKeyPattern.toString()),
        isMulti || isExactId ||
            feature_flags::gFeatureFlagUpdateOneWithoutShardKey.isEnabled(
                serverGlobalParams.featureCompatibility.acquireFCVSnapshot()));

    if (!isMulti) {
        // If the request is {multi:false} and it's not a write without shard key, then this is a
        // single op-style update which we are broadcasting to multiple shards by exact _id. Record
        // this event in our serverStatus metrics. If the query requests an upsert, then we will use
        // the two phase write protocol anyway.
        updateOneNonTargetedShardedCount.increment(1);
        if (isExactId) {
            updateOneOpStyleBroadcastWithExactIDCount.increment(1);
            if (isUpsert && useTwoPhaseWriteProtocol) {
                *useTwoPhaseWriteProtocol = true;
            } else if (!isUpsert && isNonTargetedWriteWithoutShardKeyWithExactId &&
                       feature_flags::gUpdateOneWithIdWithoutShardKey.isEnabled(
                           serverGlobalParams.featureCompatibility.acquireFCVSnapshot()) &&
                       opCtx->isRetryableWrite() && !TransactionRouter::get(opCtx)) {
                *isNonTargetedWriteWithoutShardKeyWithExactId = true;
            }
        } else {
            if (useTwoPhaseWriteProtocol) {
                *useTwoPhaseWriteProtocol = true;
            }
        }
    }

    return endPoints;
}

std::vector<ShardEndpoint> CollectionRoutingInfoTargeter::targetDelete(
    OperationContext* opCtx,
    const BatchItemRef& itemRef,
    bool* useTwoPhaseWriteProtocol,
    bool* isNonTargetedWriteWithoutShardKeyWithExactId,
    std::set<ChunkRange>* chunkRanges) const {
    const auto& deleteOp = itemRef.getDeleteRef();
    const bool isMulti = deleteOp.getMulti();
    const auto collation = write_ops::collationOf(deleteOp);

    if (isMulti) {
        deleteManyCount.increment(1);
    }

    if (!_cri.cm.isSharded()) {
        if (!isMulti) {
            deleteOneUnshardedCount.increment(1);
        }
        return {targetUnshardedCollection(_nss, _cri)};
    }

    // Collection is sharded
    auto expCtx = makeExpressionContextWithDefaultsForTargeter(opCtx,
                                                               _nss,
                                                               _cri,
                                                               collation,
                                                               boost::none,  // explain
                                                               itemRef.getLet(),
                                                               itemRef.getLegacyRuntimeConstants());

    BSONObj deleteQuery = deleteOp.getFilter();

    if (_isRequestOnTimeseriesViewNamespace) {
        uassert(ErrorCodes::IllegalOperation,
                "Cannot perform a non-multi delete on a time-series collection",
                feature_flags::gTimeseriesDeletesSupport.isEnabled(
                    serverGlobalParams.featureCompatibility.acquireFCVSnapshot()) ||
                    isMulti);

        auto tsFields = _cri.cm.getTimeseriesFields();
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
                serverGlobalParams.featureCompatibility.acquireFCVSnapshot()));
    }

    // Parse delete query.
    const auto cq = uassertStatusOKWithContext(
        _canonicalize(opCtx, expCtx, _nss, deleteQuery, collation, _cri.cm),
        str::stream() << "Could not parse delete query " << deleteQuery);

    // We first try to target based on the delete's query. It is always valid to forward any delete
    // to a single shard, so return immediately if we are able to target a single shard.
    auto endpoints = uassertStatusOK(_targetQuery(*cq, collation, chunkRanges));
    if (endpoints.size() == 1) {
        if (!deleteOp.getMulti()) {
            deleteOneTargetedShardedCount.increment(1);
        }
        return endpoints;
    }

    // We failed to target a single shard.

    // Regular single deletes must target a single shard or be exact-ID.
    // Time-series single deletes must target a single shard.
    auto isExactId = _isExactIdQuery(*cq, _cri.cm) && !_isRequestOnTimeseriesViewNamespace;
    uassert(ErrorCodes::ShardKeyNotFound,
            fmt::format("A single delete on a sharded {} contain the shard key (and have the "
                        "simple collation). Delete request: {}, shard key pattern: {}",
                        _isRequestOnTimeseriesViewNamespace
                            ? "time-series collection must"
                            : "collection must contain an exact match on _id (and have the "
                              "collection default collation) or",
                        deleteOp.toBSON().toString(),
                        _cri.cm.getShardKeyPattern().toString()),
            isMulti || isExactId ||
                feature_flags::gFeatureFlagUpdateOneWithoutShardKey.isEnabled(
                    serverGlobalParams.featureCompatibility.acquireFCVSnapshot()));

    if (!isMulti) {
        deleteOneNonTargetedShardedCount.increment(1);
        if (isExactId) {
            if (isNonTargetedWriteWithoutShardKeyWithExactId &&
                feature_flags::gUpdateOneWithIdWithoutShardKey.isEnabled(
                    serverGlobalParams.featureCompatibility.acquireFCVSnapshot()) &&
                opCtx->isRetryableWrite() && !TransactionRouter::get(opCtx)) {
                *isNonTargetedWriteWithoutShardKeyWithExactId = true;
                deleteOneWithoutShardKeyWithIdCount.increment(1);
            }
        } else {
            if (useTwoPhaseWriteProtocol) {
                *useTwoPhaseWriteProtocol = true;
            }
        }
    }

    return endpoints;
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
    expCtx->uuid = cm.getUUID();

    if (!collation.isEmpty()) {
        findCommand->setCollation(collation.getOwned());
    } else if (cm.getDefaultCollator()) {
        auto defaultCollator = cm.getDefaultCollator();
        findCommand->setCollation(defaultCollator->getSpec().toBSON());
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
    const CanonicalQuery& query,
    const BSONObj& collation,
    std::set<ChunkRange>* chunkRanges) const {

    std::set<ShardId> shardIds;
    QueryTargetingInfo info;
    try {
        getShardIdsForCanonicalQuery(query, collation, _cri.cm, &shardIds, &info);
        if (chunkRanges) {
            chunkRanges->swap(info.chunkRanges);
        }
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

StatusWith<ShardEndpoint> CollectionRoutingInfoTargeter::_targetShardKey(
    const BSONObj& shardKey, const BSONObj& collation, std::set<ChunkRange>* chunkRanges) const {
    try {
        auto chunk = _cri.cm.findIntersectingChunk(shardKey, collation);
        if (chunkRanges) {
            chunkRanges->insert(chunk.getRange());
        }
        return ShardEndpoint(
            chunk.getShardId(), _cri.getShardVersion(chunk.getShardId()), boost::none);
    } catch (const DBException& ex) {
        return ex.toStatus();
    }
    MONGO_UNREACHABLE;
}

std::vector<ShardEndpoint> CollectionRoutingInfoTargeter::targetAllShards(
    OperationContext* opCtx, std::set<ChunkRange>* chunkRanges) const {
    // This function is only called if doing a multi write that targets more than one shard. This
    // implies the collection is sharded, so we should always have a chunk manager.
    invariant(_cri.cm.isSharded());

    auto shardIds = Grid::get(opCtx)->shardRegistry()->getAllShardIds(opCtx);

    std::vector<ShardEndpoint> endpoints;
    for (auto&& shardId : shardIds) {
        ShardVersion shardVersion = _cri.getShardVersion(shardId);
        endpoints.emplace_back(std::move(shardId), std::move(shardVersion), boost::none);
    }

    if (chunkRanges) {
        _cri.cm.getAllChunkRanges(chunkRanges);
    }

    return endpoints;
}

void CollectionRoutingInfoTargeter::noteCouldNotTarget() {
    dassert(!_lastError || _lastError.value() == LastErrorType::kCouldNotTarget);
    _lastError = LastErrorType::kCouldNotTarget;
}

void CollectionRoutingInfoTargeter::noteStaleShardResponse(OperationContext* opCtx,
                                                           const ShardEndpoint& endpoint,
                                                           const StaleConfigInfo& staleInfo) {
    dassert(!_lastError || _lastError.value() == LastErrorType::kStaleShardVersion);
    Grid::get(opCtx)->catalogCache()->invalidateShardOrEntireCollectionEntryForShardedCollection(
        staleInfo.getNss(), staleInfo.getVersionWanted(), endpoint.shardName);

    if (staleInfo.getNss() != _nss) {
        // This can happen when a time-series collection becomes sharded.
        Grid::get(opCtx)
            ->catalogCache()
            ->invalidateShardOrEntireCollectionEntryForShardedCollection(
                _nss, staleInfo.getVersionWanted(), endpoint.shardName);
    }

    _lastError = LastErrorType::kStaleShardVersion;
}

void CollectionRoutingInfoTargeter::noteStaleDbResponse(OperationContext* opCtx,
                                                        const ShardEndpoint& endpoint,
                                                        const StaleDbRoutingVersion& staleInfo) {
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
    _cri = _init(opCtx, false);
    auto metadataChanged = isMetadataDifferent(lastManager, _cri);

    if (_lastError.value() == LastErrorType::kCouldNotTarget && !metadataChanged) {
        // If we couldn't target, and we didn't already update the metadata we must force a refresh
        _cri = _init(opCtx, true);
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

int CollectionRoutingInfoTargeter::getNShardsOwningChunks() const {
    if (_cri.cm.hasRoutingTable()) {
        return _cri.cm.getNShardsOwningChunks();
    }

    return 0;
}

bool CollectionRoutingInfoTargeter::isTargetedCollectionSharded() const {
    return _cri.cm.isSharded();
}

bool CollectionRoutingInfoTargeter::isTrackedTimeSeriesBucketsNamespace() const {
    // Used for testing purposes to force that we always have a tracked timeseries bucket namespace.
    if (MONGO_unlikely(isTrackedTimeSeriesBucketsNamespaceAlwaysTrue.shouldFail())) {
        return true;
    }
    return _cri.cm.hasRoutingTable() && _cri.cm.getTimeseriesFields();
}

bool CollectionRoutingInfoTargeter::timeseriesNamespaceNeedsRewrite(
    const NamespaceString& nss) const {
    return isTrackedTimeSeriesBucketsNamespace() && !nss.isTimeseriesBucketsCollection();
}

const CollectionRoutingInfo& CollectionRoutingInfoTargeter::getRoutingInfo() const {
    return _cri;
}

}  // namespace mongo
