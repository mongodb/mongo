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


#include "mongo/s/chunk_manager_targeter.h"

#include "mongo/db/commands/server_status_metric.h"
#include "mongo/db/curop.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/pipeline_d.h"
#include "mongo/db/pipeline/process_interface/mongos_process_interface.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/collation/collation_index_key.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/timeseries/timeseries_constants.h"
#include "mongo/db/timeseries/timeseries_options.h"
#include "mongo/db/timeseries/timeseries_update_delete_util.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/logv2/log.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/cluster_ddl.h"
#include "mongo/s/database_version.h"
#include "mongo/s/grid.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/str.h"
#include "signal.h"

#include "mongo/db/timeseries/timeseries_update_delete_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

enum CompareResult { CompareResult_Unknown, CompareResult_GTE, CompareResult_LT };

constexpr auto kIdFieldName = "_id"_sd;

const ShardKeyPattern kVirtualIdShardKey(BSON(kIdFieldName << 1));

using UpdateType = write_ops::UpdateModification::Type;

// Tracks the number of {multi:false} updates with an exact match on _id that are broadcasted to
// multiple shards.
CounterMetric updateOneOpStyleBroadcastWithExactIDCount(
    "query.updateOneOpStyleBroadcastWithExactIDCount");

/**
 * Update expressions are bucketed into one of two types for the purposes of shard targeting:
 *
 * Replacement style: coll.update({ x : 1 }, { y : 2 })
 * OpStyle: coll.update({ x : 1 }, { $set : { y : 2 } })
 *            or
 *          coll.update({x: 1}, [{$addFields: {y: 2}}])
 */
void validateUpdateDoc(const write_ops::UpdateOpEntry& updateDoc) {
    const auto& updateMod = updateDoc.getU();
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
            updateType == UpdateType::kModifier || !updateDoc.getMulti());
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
    const auto idFromQuery =
        uassertStatusOK(kVirtualIdShardKey.extractShardKeyFromQuery(expCtx, updateQuery));
    if (auto idElt = idFromQuery[kIdFieldName]) {
        updateExpr = updateExpr.addField(idElt);
    }

    return updateExpr;
}

/**
 * This returns "does the query have an _id field" and "is the _id field querying for a direct
 * value like _id : 3 and not _id : { $gt : 3 }"
 *
 * If the query does not use the collection default collation, the _id field cannot contain strings,
 * objects, or arrays.
 *
 * Ex: { _id : 1 } => true
 *     { foo : <anything>, _id : 1 } => true
 *     { _id : { $lt : 30 } } => false
 *     { foo : <anything> } => false
 */
bool isExactIdQuery(OperationContext* opCtx, const CanonicalQuery& query, const ChunkManager& cm) {
    auto shardKey = kVirtualIdShardKey.extractShardKeyFromQuery(query);
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

bool isExactIdQuery(OperationContext* opCtx,
                    const NamespaceString& nss,
                    const BSONObj query,
                    const BSONObj collation,
                    const ChunkManager& cm) {
    auto findCommand = std::make_unique<FindCommandRequest>(nss);
    findCommand->setFilter(query);
    if (!collation.isEmpty()) {
        findCommand->setCollation(collation);
    }
    const auto cq = CanonicalQuery::canonicalize(opCtx,
                                                 std::move(findCommand),
                                                 false, /* isExplain */
                                                 nullptr,
                                                 ExtensionsCallbackNoop(),
                                                 MatchExpressionParser::kAllowAllSpecialFeatures);

    return cq.isOK() && isExactIdQuery(opCtx, *cq.getValue(), cm);
}

/**
 * Whether or not the manager/primary pair is different from the other manager/primary pair.
 */
bool isMetadataDifferent(const ChunkManager& managerA, const ChunkManager& managerB) {
    if ((managerA.isSharded() && !managerB.isSharded()) ||
        (!managerA.isSharded() && managerB.isSharded()))
        return true;

    if (managerA.isSharded()) {
        return managerA.getVersion() != managerB.getVersion();
    }

    return managerA.dbVersion() != managerB.dbVersion();
}

}  // namespace

ChunkManagerTargeter::ChunkManagerTargeter(OperationContext* opCtx,
                                           const NamespaceString& nss,
                                           boost::optional<OID> targetEpoch)
    : _nss(nss), _targetEpoch(std::move(targetEpoch)), _cm(_init(opCtx, false)) {}

/**
 * Initializes and returns the ChunkManger which needs to be used for targeting.
 * If 'refresh' is true, additionally fetches the latest routing info from the config servers.
 *
 * Note: For sharded time-series collections, we use the buckets collection for targeting. If the
 * user request is on the view namespace, we implicity tranform the request to the buckets namepace.
 */
ChunkManager ChunkManagerTargeter::_init(OperationContext* opCtx, bool refresh) {
    cluster::createDatabase(opCtx, _nss.db());

    if (refresh) {
        uassertStatusOK(
            Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfoWithRefresh(opCtx, _nss));
    }
    auto cm = uassertStatusOK(getCollectionRoutingInfoForTxnCmd(opCtx, _nss));

    // For a sharded time-series collection, only the underlying buckets collection is stored on the
    // config servers. If the user operation is on the time-series view namespace, we should check
    // if the buckets namespace is sharded. There are a few cases that we need to take care of,
    // 1. The request is on the view namespace. We check if the buckets collection is sharded. If
    //    it is, we use the buckets collection namespace for the purpose of trageting. Additionally,
    //    we set the '_isRequestOnTimeseriesViewNamespace' to true for this case.
    // 2. If request is on the buckets namespace, we don't need to execute any additional
    //    time-series logic. We can treat the request as though it was a request on a regular
    //    collection.
    // 3. During a cache refresh a the buckets collection changes from sharded to unsharded. In this
    //    case, if the original request is on the view namespace, then we should reset the namespace
    //    back to the view namespace and reset '_isRequestOnTimeseriesViewNamespace'.
    if (!cm.isSharded() && !_nss.isTimeseriesBucketsCollection()) {
        auto bucketsNs = _nss.makeTimeseriesBucketsNamespace();
        if (refresh) {
            uassertStatusOK(Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfoWithRefresh(
                opCtx, bucketsNs));
        }
        auto bucketsRoutingInfo =
            uassertStatusOK(getCollectionRoutingInfoForTxnCmd(opCtx, bucketsNs));
        if (bucketsRoutingInfo.isSharded()) {
            _nss = bucketsNs;
            cm = std::move(bucketsRoutingInfo);
            _isRequestOnTimeseriesViewNamespace = true;
        }
    } else if (!cm.isSharded() && _isRequestOnTimeseriesViewNamespace) {
        // This can happen if a sharded time-series collection is dropped and re-created. Then we
        // need to reset the namepace to the original namespace.
        _nss = _nss.getTimeseriesViewNamespace();

        if (refresh) {
            uassertStatusOK(
                Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfoWithRefresh(opCtx, _nss));
        }
        cm = uassertStatusOK(getCollectionRoutingInfoForTxnCmd(opCtx, _nss));
        _isRequestOnTimeseriesViewNamespace = false;
    }

    if (_targetEpoch) {
        uassert(ErrorCodes::StaleEpoch, "Collection has been dropped", cm.isSharded());
        uassert(ErrorCodes::StaleEpoch,
                "Collection epoch has changed",
                cm.getVersion().epoch() == *_targetEpoch);
    }
    return cm;
}

const NamespaceString& ChunkManagerTargeter::getNS() const {
    return _nss;
}

BSONObj ChunkManagerTargeter::extractBucketsShardKeyFromTimeseriesDoc(
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
            builder.appendAs(metaElement, timeseries::kBucketMetaFieldName);
        }
    }

    auto docWithShardKey = builder.obj();
    return pattern.extractShardKeyFromDoc(docWithShardKey);
}

ShardEndpoint ChunkManagerTargeter::targetInsert(OperationContext* opCtx,
                                                 const BSONObj& doc) const {
    BSONObj shardKey;

    if (_cm.isSharded()) {
        const auto& shardKeyPattern = _cm.getShardKeyPattern();
        if (_isRequestOnTimeseriesViewNamespace) {
            auto tsFields = _cm.getTimeseriesFields();
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
    }

    // Target the shard key or database primary
    if (!shardKey.isEmpty()) {
        return uassertStatusOK(_targetShardKey(shardKey, CollationSpec::kSimpleSpec));
    }

    // TODO (SERVER-51070): Remove the boost::none when the config server can support shardVersion
    // in commands
    return ShardEndpoint(
        _cm.dbPrimary(),
        _nss.isOnInternalDb() ? boost::optional<ChunkVersion>() : ChunkVersion::UNSHARDED(),
        _nss.isOnInternalDb() ? boost::optional<DatabaseVersion>() : _cm.dbVersion());
}

std::vector<ShardEndpoint> ChunkManagerTargeter::targetUpdate(OperationContext* opCtx,
                                                              const BatchItemRef& itemRef) const {
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
    // as if the the shard key values are specified as NULL. A replacement document is also allowed
    // to have a missing '_id', and if the '_id' exists in the query, it will be emplaced in the
    // replacement document for targeting purposes.

    const auto& updateOp = itemRef.getUpdate();

    // If the collection is not sharded, forward the update to the primary shard.
    if (!_cm.isSharded()) {
        // TODO (SERVER-51070): Remove the boost::none when the config server can support
        // shardVersion in commands
        return std::vector{ShardEndpoint(
            _cm.dbPrimary(),
            _nss.isOnInternalDb() ? boost::optional<ChunkVersion>() : ChunkVersion::UNSHARDED(),
            _nss.isOnInternalDb() ? boost::optional<DatabaseVersion>() : _cm.dbVersion())};
    }

    const auto& shardKeyPattern = _cm.getShardKeyPattern();
    const auto collation = write_ops::collationOf(updateOp);

    auto expCtx = makeExpressionContextWithDefaultsForTargeter(opCtx,
                                                               _nss,
                                                               collation,
                                                               boost::none,  // explain
                                                               itemRef.getLet(),
                                                               itemRef.getLegacyRuntimeConstants());

    const bool isUpsert = updateOp.getUpsert();
    auto query = updateOp.getQ();

    if (_isRequestOnTimeseriesViewNamespace) {
        uassert(ErrorCodes::NotImplemented,
                str::stream() << "Updates are disallowed on sharded timeseries collections.",
                feature_flags::gFeatureFlagShardedTimeSeriesUpdateDelete.isEnabledAndIgnoreFCV());
        uassert(ErrorCodes::InvalidOptions,
                str::stream()
                    << "A {multi:false} update on a sharded timeseries collection is disallowed.",
                updateOp.getMulti());
        uassert(ErrorCodes::InvalidOptions,
                str::stream()
                    << "An {upsert:true} update on a sharded timeseries collection is disallowed.",
                !isUpsert);

        // Since this is a timeseries query, we may need to rename the metaField.
        if (auto metaField = _cm.getTimeseriesFields().get().getMetaField()) {
            query = timeseries::translateQuery(query, *metaField);
        } else {
            // We want to avoid targeting the query incorrectly if no metaField is defined on the
            // timeseries collection, since we only allow queries on the metaField for timeseries
            // updates. Note: any non-empty query should fail to update once it reaches the shards
            // because there is no metaField for it to query for, but we don't want to validate this
            // during routing.
            query = BSONObj();
        }
    }

    validateUpdateDoc(updateOp);
    const auto updateExpr =
        getUpdateExprForTargeting(expCtx, shardKeyPattern, query, updateOp.getU());

    // Utility function to target an update by shard key, and to handle any potential error results.
    auto targetByShardKey = [this, &collation](StatusWith<BSONObj> swShardKey, std::string msg) {
        const auto& shardKey = uassertStatusOKWithContext(std::move(swShardKey), msg);
        uassert(ErrorCodes::ShardKeyNotFound,
                str::stream() << msg << " :: could not extract exact shard key",
                !shardKey.isEmpty());
        return std::vector{uassertStatusOKWithContext(_targetShardKey(shardKey, collation), msg)};
    };

    // If this is an upsert, then the query must contain an exact match on the shard key. If we were
    // to target based on the replacement doc, it could result in an insertion even if a document
    // matching the query exists on another shard.
    if (isUpsert) {
        return targetByShardKey(shardKeyPattern.extractShardKeyFromQuery(expCtx, query),
                                "Failed to target upsert by query");
    }

    // We first try to target based on the update's query. It is always valid to forward any update
    // or upsert to a single shard, so return immediately if we are able to target a single shard.
    auto endPoints = uassertStatusOK(_targetQuery(expCtx, query, collation));
    if (endPoints.size() == 1) {
        return endPoints;
    }

    // Replacement-style updates must always target a single shard. If we were unable to do so using
    // the query, we attempt to extract the shard key from the replacement and target based on it.
    if (updateOp.getU().type() == write_ops::UpdateModification::Type::kReplacement) {
        return targetByShardKey(shardKeyPattern.extractShardKeyFromDoc(updateExpr),
                                "Failed to target update by replacement document");
    }

    // If we are here then this is an op-style update and we were not able to target a single shard.
    // Non-multi updates must target a single shard or an exact _id.
    uassert(ErrorCodes::InvalidOptions,
            str::stream()
                << "A {multi:false} update on a sharded collection must contain an "
                   "exact match on _id (and have the collection default collation) or target a "
                   "single shard (and have the simple collation), but this update targeted "
                << endPoints.size() << " shards. Update request: " << updateOp.toBSON()
                << ", shard key pattern: " << shardKeyPattern.toString(),
            updateOp.getMulti() || isExactIdQuery(opCtx, _nss, query, collation, _cm));

    // If the request is {multi:false}, then this is a single op-style update which we are
    // broadcasting to multiple shards by exact _id. Record this event in our serverStatus metrics.
    if (!updateOp.getMulti()) {
        updateOneOpStyleBroadcastWithExactIDCount.increment(1);
    }

    return endPoints;
}

std::vector<ShardEndpoint> ChunkManagerTargeter::targetDelete(OperationContext* opCtx,
                                                              const BatchItemRef& itemRef) const {
    const auto& deleteOp = itemRef.getDelete();
    const auto collation = write_ops::collationOf(deleteOp);

    auto expCtx = makeExpressionContextWithDefaultsForTargeter(opCtx,
                                                               _nss,
                                                               collation,
                                                               boost::none,  // explain
                                                               itemRef.getLet(),
                                                               itemRef.getLegacyRuntimeConstants());

    BSONObj deleteQuery = deleteOp.getQ();
    BSONObj shardKey;
    if (_cm.isSharded()) {
        if (_isRequestOnTimeseriesViewNamespace) {
            uassert(ErrorCodes::NotImplemented,
                    "Deletes on sharded time-series collections feature is not enabled",
                    feature_flags::gFeatureFlagShardedTimeSeriesUpdateDelete.isEnabled(
                        serverGlobalParams.featureCompatibility));

            uassert(ErrorCodes::IllegalOperation,
                    "Cannot perform a non-multi delete on a time-series collection",
                    deleteOp.getMulti());

            auto tsFields = _cm.getTimeseriesFields();
            tassert(5918101, "Missing timeseriesFields on buckets collection", tsFields);

            const auto& metaField = tsFields->getMetaField();
            if (metaField) {
                // Translate delete query into the query to the time-series buckets collection.
                deleteQuery = timeseries::translateQuery(deleteQuery, *metaField);
            } else {
                // In case the time-series collection does not have meta field defined, we target
                // the request to all shards using empty predicate. Since we allow only delete
                // requests with 'limit:0', we will not delete any extra documents.
                deleteQuery = BSONObj();
            }
        }

        // Sharded collections have the following further requirements for targeting:
        //
        // Limit-1 deletes must be targeted exactly by shard key *or* exact _id
        shardKey =
            uassertStatusOK(_cm.getShardKeyPattern().extractShardKeyFromQuery(expCtx, deleteQuery));
    }

    // Target the shard key or delete query
    if (!shardKey.isEmpty()) {
        auto swEndpoint = _targetShardKey(shardKey, collation);
        if (swEndpoint.isOK()) {
            return std::vector{std::move(swEndpoint.getValue())};
        }
    }

    // We failed to target a single shard.

    // Parse delete query.
    auto findCommand = std::make_unique<FindCommandRequest>(_nss);
    findCommand->setFilter(deleteQuery);
    if (!collation.isEmpty()) {
        findCommand->setCollation(collation);
    }
    auto cq = uassertStatusOKWithContext(
        CanonicalQuery::canonicalize(opCtx,
                                     std::move(findCommand),
                                     false, /* isExplain */
                                     expCtx,
                                     ExtensionsCallbackNoop(),
                                     MatchExpressionParser::kAllowAllSpecialFeatures),
        str::stream() << "Could not parse delete query " << deleteQuery);

    // Single deletes must target a single shard or be exact-ID.
    uassert(ErrorCodes::ShardKeyNotFound,
            str::stream() << "A single delete on a sharded collection must contain an exact match "
                             "on _id (and have the collection default collation) or contain the "
                             "shard key (and have the simple collation). Delete request: "
                          << deleteOp.toBSON()
                          << ", shard key pattern: " << _cm.getShardKeyPattern().toString(),
            !_cm.isSharded() || deleteOp.getMulti() || isExactIdQuery(opCtx, *cq, _cm));

    return uassertStatusOK(_targetQuery(expCtx, deleteQuery, collation));
}

StatusWith<std::vector<ShardEndpoint>> ChunkManagerTargeter::_targetQuery(
    boost::intrusive_ptr<ExpressionContext> expCtx,
    const BSONObj& query,
    const BSONObj& collation) const {
    if (!_cm.isSharded()) {
        // TODO (SERVER-51070): Remove the boost::none when the config server can support
        // shardVersion in commands
        return std::vector{ShardEndpoint(
            _cm.dbPrimary(),
            _nss.isOnInternalDb() ? boost::optional<ChunkVersion>() : ChunkVersion::UNSHARDED(),
            _nss.isOnInternalDb() ? boost::optional<DatabaseVersion>() : _cm.dbVersion())};
    }

    std::set<ShardId> shardIds;
    try {
        _cm.getShardIdsForQuery(expCtx, query, collation, &shardIds);
    } catch (const DBException& ex) {
        return ex.toStatus();
    }

    std::vector<ShardEndpoint> endpoints;
    for (auto&& shardId : shardIds) {
        endpoints.emplace_back(std::move(shardId), _cm.getVersion(shardId), boost::none);
    }

    return endpoints;
}

StatusWith<ShardEndpoint> ChunkManagerTargeter::_targetShardKey(const BSONObj& shardKey,
                                                                const BSONObj& collation) const {
    try {
        auto chunk = _cm.findIntersectingChunk(shardKey, collation);
        return ShardEndpoint(chunk.getShardId(), _cm.getVersion(chunk.getShardId()), boost::none);
    } catch (const DBException& ex) {
        return ex.toStatus();
    }
    MONGO_UNREACHABLE;
}

std::vector<ShardEndpoint> ChunkManagerTargeter::targetAllShards(OperationContext* opCtx) const {
    // This function is only called if doing a multi write that targets more than one shard. This
    // implies the collection is sharded, so we should always have a chunk manager.
    invariant(_cm.isSharded());

    auto shardIds = Grid::get(opCtx)->shardRegistry()->getAllShardIds(opCtx);

    std::vector<ShardEndpoint> endpoints;
    for (auto&& shardId : shardIds) {
        endpoints.emplace_back(std::move(shardId), _cm.getVersion(shardId), boost::none);
    }

    return endpoints;
}

void ChunkManagerTargeter::noteCouldNotTarget() {
    dassert(!_lastError || _lastError.get() == LastErrorType::kCouldNotTarget);
    _lastError = LastErrorType::kCouldNotTarget;
}

void ChunkManagerTargeter::noteStaleShardResponse(OperationContext* opCtx,
                                                  const ShardEndpoint& endpoint,
                                                  const StaleConfigInfo& staleInfo) {
    dassert(!_lastError || _lastError.get() == LastErrorType::kStaleShardVersion);
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

void ChunkManagerTargeter::noteStaleDbResponse(OperationContext* opCtx,
                                               const ShardEndpoint& endpoint,
                                               const StaleDbRoutingVersion& staleInfo) {
    dassert(!_lastError || _lastError.get() == LastErrorType::kStaleDbVersion);
    Grid::get(opCtx)->catalogCache()->onStaleDatabaseVersion(_nss.db(),
                                                             staleInfo.getVersionWanted());
    _lastError = LastErrorType::kStaleDbVersion;
}

bool ChunkManagerTargeter::refreshIfNeeded(OperationContext* opCtx) {
    // Did we have any stale config or targeting errors at all?
    if (!_lastError) {
        return false;
    }

    // Make sure that even in case of exception we will clear the last error.
    ON_BLOCK_EXIT([&] { _lastError = boost::none; });

    LOGV2_DEBUG(22912,
                4,
                "ChunkManagerTargeter checking if refresh is needed",
                "couldNotTarget"_attr = _lastError.get() == LastErrorType::kCouldNotTarget,
                "staleShardVersion"_attr = _lastError.get() == LastErrorType::kStaleShardVersion,
                "staleDbVersion"_attr = _lastError.get() == LastErrorType::kStaleDbVersion);

    // Get the latest metadata information from the cache if there were issues
    auto lastManager = _cm;
    _cm = _init(opCtx, false);
    auto metadataChanged = isMetadataDifferent(lastManager, _cm);

    if (_lastError.get() == LastErrorType::kCouldNotTarget && !metadataChanged) {
        // If we couldn't target and we dind't already update the metadata we must force a refresh
        _cm = _init(opCtx, true);
        metadataChanged = isMetadataDifferent(lastManager, _cm);
    }

    return metadataChanged;
}

int ChunkManagerTargeter::getNShardsOwningChunks() const {
    if (_cm.isSharded()) {
        return _cm.getNShardsOwningChunks();
    }

    return 0;
}

bool ChunkManagerTargeter::isShardedTimeSeriesBucketsNamespace() const {
    return _cm.isSharded() && _cm.getTimeseriesFields();
}

bool ChunkManagerTargeter::timeseriesNamespaceNeedsRewrite(const NamespaceString& nss) const {
    return isShardedTimeSeriesBucketsNamespace() && !nss.isTimeseriesBucketsCollection();
}

const ChunkManager& ChunkManagerTargeter::getRoutingInfo() const {
    return _cm;
}

}  // namespace mongo
