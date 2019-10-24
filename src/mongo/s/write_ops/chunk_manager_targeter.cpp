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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/s/write_ops/chunk_manager_targeter.h"

#include "mongo/base/counter.h"
#include "mongo/db/commands/server_status_metric.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/collation/collation_index_key.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/database_version_helpers.h"
#include "mongo/s/grid.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/util/log.h"
#include "mongo/util/str.h"

namespace mongo {
namespace {

enum CompareResult { CompareResult_Unknown, CompareResult_GTE, CompareResult_LT };

constexpr auto kIdFieldName = "_id"_sd;

const ShardKeyPattern kVirtualIdShardKey(BSON(kIdFieldName << 1));

using UpdateType = ChunkManagerTargeter::UpdateType;

// Tracks the number of {multi:false} updates with an exact match on _id that are broadcasted to
// multiple shards.
Counter64 updateOneOpStyleBroadcastWithExactIDCount;
ServerStatusMetricField<Counter64> updateOneOpStyleBroadcastWithExactIDStats(
    "query.updateOneOpStyleBroadcastWithExactIDCount", &updateOneOpStyleBroadcastWithExactIDCount);

/**
 * Update expressions are bucketed into one of two types for the purposes of shard targeting:
 *
 * Replacement style: coll.update({ x : 1 }, { y : 2 })
 * OpStyle: coll.update({ x : 1 }, { $set : { y : 2 } })
 *            or
 *          coll.update({x: 1}, [{$addFields: {y: 2}}])
 */
StatusWith<UpdateType> getUpdateExprType(const write_ops::UpdateOpEntry& updateDoc) {
    const auto updateMod = updateDoc.getU();
    if (updateMod.type() == write_ops::UpdateModification::Type::kPipeline) {
        return UpdateType::kOpStyle;
    }

    // Obtain the update expression from the request.
    const auto& updateExpr = updateMod.getUpdateClassic();

    // Empty update is replacement-style by default.
    auto updateType = (updateExpr.isEmpty() ? UpdateType::kReplacement : UpdateType::kUnknown);

    // Make sure that the update expression does not mix $op and non-$op fields.
    for (const auto& curField : updateExpr) {
        const auto curFieldType =
            (curField.fieldNameStringData()[0] == '$' ? UpdateType::kOpStyle
                                                      : UpdateType::kReplacement);

        // If the current field's type does not match the existing updateType, abort.
        if (updateType != curFieldType && updateType != UpdateType::kUnknown) {
            return {ErrorCodes::UnsupportedFormat,
                    str::stream() << "update document " << updateExpr
                                  << " has mixed $operator and non-$operator style fields"};
        }
        updateType = curFieldType;
    }

    if (updateType == UpdateType::kReplacement && updateDoc.getMulti()) {
        return {ErrorCodes::InvalidOptions, "Replacement-style updates cannot be {multi:true}"};
    }

    return updateType;
}

/**
 * Obtain the update expression from the given update doc. If this is a replacement-style update,
 * and the shard key includes _id but the replacement document does not, we attempt to find an exact
 * _id match in the query component and add it to the doc. We do this because mongoD will propagate
 * _id from the existing document if this is an update, and will extract _id from the query when
 * generating the new document in the case of an upsert. It is therefore always correct to target
 * the operation on the basis of the combined updateExpr and query.
 */
StatusWith<BSONObj> getUpdateExprForTargeting(OperationContext* opCtx,
                                              const ShardKeyPattern& shardKeyPattern,
                                              const UpdateType updateType,
                                              const write_ops::UpdateOpEntry& updateDoc) {
    // We should never see an invalid update type here.
    invariant(updateType != UpdateType::kUnknown);

    // If this is not a replacement update, then the update expression remains unchanged.
    if (updateType != UpdateType::kReplacement) {
        const auto& updateMod = updateDoc.getU();
        BSONObjBuilder objBuilder;
        updateMod.serializeToBSON("u", &objBuilder);
        return objBuilder.obj();
    }

    // Extract the raw update expression from the request.
    const auto& updateMod = updateDoc.getU();
    invariant(updateMod.type() == write_ops::UpdateModification::Type::kClassic);
    auto updateExpr = updateMod.getUpdateClassic();

    // Replace any non-existent shard key values with a null value.
    updateExpr = shardKeyPattern.emplaceMissingShardKeyValuesForDocument(updateExpr);

    // If we aren't missing _id, return the update expression as-is.
    if (updateExpr.hasField(kIdFieldName)) {
        return updateExpr;
    }

    // We are missing _id, so attempt to extract it from an exact match in the update's query spec.
    // This will guarantee that we can target a single shard, but it is not necessarily fatal if no
    // exact _id can be found.
    const auto idFromQuery = kVirtualIdShardKey.extractShardKeyFromQuery(opCtx, updateDoc.getQ());
    if (!idFromQuery.isOK()) {
        return idFromQuery;
    } else if (auto idElt = idFromQuery.getValue()[kIdFieldName]) {
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
bool isExactIdQuery(OperationContext* opCtx, const CanonicalQuery& query, ChunkManager* manager) {
    auto shardKey = kVirtualIdShardKey.extractShardKeyFromQuery(query);
    BSONElement idElt = shardKey["_id"];

    if (!idElt) {
        return false;
    }

    if (CollationIndexKey::isCollatableType(idElt.type()) && manager &&
        !query.getQueryRequest().getCollation().isEmpty() &&
        !CollatorInterface::collatorsMatch(query.getCollator(), manager->getDefaultCollator())) {

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
                    ChunkManager* manager) {
    auto qr = std::make_unique<QueryRequest>(nss);
    qr->setFilter(query);
    if (!collation.isEmpty()) {
        qr->setCollation(collation);
    }
    const auto cq = CanonicalQuery::canonicalize(opCtx,
                                                 std::move(qr),
                                                 nullptr,
                                                 ExtensionsCallbackNoop(),
                                                 MatchExpressionParser::kAllowAllSpecialFeatures);

    return cq.isOK() && isExactIdQuery(opCtx, *cq.getValue(), manager);
}
//
// Utilities to compare shard and db versions
//

/**
 * Returns the relationship of two shard versions. Shard versions of a collection that has not
 * been dropped and recreated and where there is at least one chunk on a shard are comparable,
 * otherwise the result is ambiguous.
 */
CompareResult compareShardVersions(const ChunkVersion& shardVersionA,
                                   const ChunkVersion& shardVersionB) {
    // Collection may have been dropped
    if (shardVersionA.epoch() != shardVersionB.epoch()) {
        return CompareResult_Unknown;
    }

    // Zero shard versions are only comparable to themselves
    if (!shardVersionA.isSet() || !shardVersionB.isSet()) {
        // If both are zero...
        if (!shardVersionA.isSet() && !shardVersionB.isSet()) {
            return CompareResult_GTE;
        }

        return CompareResult_Unknown;
    }

    if (shardVersionA < shardVersionB)
        return CompareResult_LT;
    else
        return CompareResult_GTE;
}

ChunkVersion getShardVersion(const CachedCollectionRoutingInfo& routingInfo,
                             const ShardId& shardId) {
    if (routingInfo.cm()) {
        return routingInfo.cm()->getVersion(shardId);
    }

    return ChunkVersion::UNSHARDED();
}

/**
 * Returns the relationship between two maps of shard versions. As above, these maps are often
 * comparable when the collection has not been dropped and there is at least one chunk on the
 * shards. If any versions in the maps are not comparable, the result is _Unknown.
 *
 * If any versions in the first map (cached) are _LT the versions in the second map (remote),
 * the first (cached) versions are _LT the second (remote) versions.
 *
 * Note that the signature here is weird since our cached map of chunk versions is stored in a
 * ChunkManager or is implicit in the primary shard of the collection.
 */
CompareResult compareAllShardVersions(const CachedCollectionRoutingInfo& routingInfo,
                                      const ShardVersionMap& remoteShardVersions) {
    CompareResult finalResult = CompareResult_GTE;

    for (const auto& shardVersionEntry : remoteShardVersions) {
        const ShardId& shardId = shardVersionEntry.first;
        const ChunkVersion& remoteShardVersion = shardVersionEntry.second;

        ChunkVersion cachedShardVersion;

        try {
            // Throws b/c shard constructor throws
            cachedShardVersion = getShardVersion(routingInfo, shardId);
        } catch (const DBException& ex) {
            warning() << "could not lookup shard " << shardId
                      << " in local cache, shard metadata may have changed"
                      << " or be unavailable" << causedBy(ex);

            return CompareResult_Unknown;
        }

        // Compare the remote and cached versions
        CompareResult result = compareShardVersions(cachedShardVersion, remoteShardVersion);

        if (result == CompareResult_Unknown)
            return result;

        if (result == CompareResult_LT)
            finalResult = CompareResult_LT;

        // Note that we keep going after _LT b/c there could be more _Unknowns.
    }

    return finalResult;
}

CompareResult compareDbVersions(const CachedCollectionRoutingInfo& routingInfo,
                                const DatabaseVersion& remoteDbVersion) {
    DatabaseVersion cachedDbVersion = routingInfo.db().databaseVersion();

    // Db may have been dropped
    if (cachedDbVersion.getUuid() != remoteDbVersion.getUuid()) {
        return CompareResult_Unknown;
    }

    // Db may have been moved
    if (cachedDbVersion.getLastMod() < remoteDbVersion.getLastMod()) {
        return CompareResult_LT;
    }

    return CompareResult_GTE;
}

/**
 * Whether or not the manager/primary pair is different from the other manager/primary pair.
 */
bool isMetadataDifferent(const std::shared_ptr<ChunkManager>& managerA,
                         const DatabaseVersion dbVersionA,
                         const std::shared_ptr<ChunkManager>& managerB,
                         const DatabaseVersion dbVersionB) {
    if ((managerA && !managerB) || (!managerA && managerB))
        return true;

    if (managerA) {
        return managerA->getVersion() != managerB->getVersion();
    }

    return databaseVersion::equal(dbVersionA, dbVersionB);
}

/**
 * Whether or not the manager/primary pair was changed or refreshed from a previous version
 * of the metadata.
 */
bool wasMetadataRefreshed(const std::shared_ptr<ChunkManager>& managerA,
                          const DatabaseVersion dbVersionA,
                          const std::shared_ptr<ChunkManager>& managerB,
                          const DatabaseVersion dbVersionB) {
    if (isMetadataDifferent(managerA, dbVersionA, managerB, dbVersionB))
        return true;

    if (managerA) {
        dassert(managerB.get());  // otherwise metadata would be different
        return managerA->getSequenceNumber() != managerB->getSequenceNumber();
    }

    return false;
}

}  // namespace

ChunkManagerTargeter::ChunkManagerTargeter(const NamespaceString& nss,
                                           boost::optional<OID> targetEpoch)
    : _nss(nss), _needsTargetingRefresh(false), _targetEpoch(targetEpoch) {}


Status ChunkManagerTargeter::init(OperationContext* opCtx) {
    try {
        createShardDatabase(opCtx, _nss.db());
    } catch (const DBException& e) {
        return e.toStatus();
    }

    const auto routingInfoStatus = getCollectionRoutingInfoForTxnCmd(opCtx, _nss);
    if (!routingInfoStatus.isOK()) {
        return routingInfoStatus.getStatus();
    }

    _routingInfo = std::move(routingInfoStatus.getValue());

    if (_targetEpoch) {
        uassert(ErrorCodes::StaleEpoch, "Collection has been dropped", _routingInfo->cm());
        uassert(ErrorCodes::StaleEpoch,
                "Collection has been dropped and recreated",
                _routingInfo->cm()->getVersion().epoch() == *_targetEpoch);
    }

    return Status::OK();
}

const NamespaceString& ChunkManagerTargeter::getNS() const {
    return _nss;
}

StatusWith<ShardEndpoint> ChunkManagerTargeter::targetInsert(OperationContext* opCtx,
                                                             const BSONObj& doc) const {
    BSONObj shardKey;

    if (_routingInfo->cm()) {
        shardKey = _routingInfo->cm()->getShardKeyPattern().extractShardKeyFromDoc(doc);
    }

    // Target the shard key or database primary
    if (!shardKey.isEmpty()) {
        return _targetShardKey(shardKey, CollationSpec::kSimpleSpec, doc.objsize());
    } else {
        if (!_routingInfo->db().primary()) {
            return Status(ErrorCodes::NamespaceNotFound,
                          str::stream() << "could not target insert in collection " << getNS().ns()
                                        << "; no metadata found");
        }

        return ShardEndpoint(_routingInfo->db().primary()->getId(),
                             ChunkVersion::UNSHARDED(),
                             _routingInfo->db().databaseVersion());
    }

    return Status::OK();
}

StatusWith<std::vector<ShardEndpoint>> ChunkManagerTargeter::targetUpdate(
    OperationContext* opCtx, const write_ops::UpdateOpEntry& updateDoc) const {
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
    const auto updateType = getUpdateExprType(updateDoc);
    if (!updateType.isOK()) {
        return updateType.getStatus();
    }

    // If the collection is not sharded, forward the update to the primary shard.
    if (!_routingInfo->cm()) {
        if (!_routingInfo->db().primary()) {
            return {ErrorCodes::NamespaceNotFound,
                    str::stream() << "could not target update on " << getNS().ns()
                                  << "; no metadata found"};
        }
        return std::vector<ShardEndpoint>{{_routingInfo->db().primaryId(),
                                           ChunkVersion::UNSHARDED(),
                                           _routingInfo->db().databaseVersion()}};
    }

    const auto& shardKeyPattern = _routingInfo->cm()->getShardKeyPattern();
    const auto collation = write_ops::collationOf(updateDoc);

    const auto updateExpr =
        getUpdateExprForTargeting(opCtx, shardKeyPattern, updateType.getValue(), updateDoc);
    const bool isUpsert = updateDoc.getUpsert();
    const auto query = updateDoc.getQ();
    if (!updateExpr.isOK()) {
        return updateExpr.getStatus();
    }

    // Utility function to target an update by shard key, and to handle any potential error results.
    const auto targetByShardKey = [&collation,
                                   this](StatusWith<BSONObj> shardKey,
                                         StringData msg) -> StatusWith<std::vector<ShardEndpoint>> {
        if (!shardKey.isOK()) {
            return shardKey.getStatus().withContext(msg);
        }
        if (shardKey.getValue().isEmpty()) {
            return {ErrorCodes::ShardKeyNotFound,
                    str::stream() << msg << " :: could not extract exact shard key"};
        }
        auto swEndPoint = _targetShardKey(shardKey.getValue(), collation, 0);
        if (!swEndPoint.isOK()) {
            return swEndPoint.getStatus().withContext(msg);
        }
        return {{swEndPoint.getValue()}};
    };

    // If this is an upsert, then the query must contain an exact match on the shard key. If we were
    // to target based on the replacement doc, it could result in an insertion even if a document
    // matching the query exists on another shard.
    if (isUpsert) {
        return targetByShardKey(shardKeyPattern.extractShardKeyFromQuery(opCtx, query),
                                "Failed to target upsert by query");
    }

    // We first try to target based on the update's query. It is always valid to forward any update
    // or upsert to a single shard, so return immediately if we are able to target a single shard.
    auto shardEndPoints = _targetQuery(opCtx, query, collation);
    if (!shardEndPoints.isOK() || shardEndPoints.getValue().size() == 1) {
        return shardEndPoints;
    }

    // Replacement-style updates must always target a single shard. If we were unable to do so using
    // the query, we attempt to extract the shard key from the replacement and target based on it.
    if (updateType == UpdateType::kReplacement) {
        return targetByShardKey(shardKeyPattern.extractShardKeyFromDoc(updateExpr.getValue()),
                                "Failed to target update by replacement document");
    }

    // If we are here then this is an op-style update and we were not able to target a single shard.
    // Non-multi updates must target a single shard or an exact _id.
    if (!updateDoc.getMulti() &&
        !isExactIdQuery(opCtx, getNS(), query, collation, _routingInfo->cm().get())) {
        return {ErrorCodes::InvalidOptions,
                str::stream() << "A {multi:false} update on a sharded collection must either "
                                 "contain an exact match on _id or must target a single shard but "
                                 "this update targeted _id (and have the collection default "
                                 "collation) or must target a single shard (and have the simple "
                                 "collation), but this update targeted "
                              << shardEndPoints.getValue().size()
                              << " shards. Update request: " << updateDoc.toBSON()
                              << ", shard key pattern: " << shardKeyPattern.toString()};
    }

    // If the request is {multi:false}, then this is a single op-style update which we are
    // broadcasting to multiple shards by exact _id. Record this event in our serverStatus metrics.
    if (!updateDoc.getMulti()) {
        updateOneOpStyleBroadcastWithExactIDCount.increment(1);
    }
    return shardEndPoints;
}

StatusWith<std::vector<ShardEndpoint>> ChunkManagerTargeter::targetDelete(
    OperationContext* opCtx, const write_ops::DeleteOpEntry& deleteDoc) const {
    BSONObj shardKey;

    if (_routingInfo->cm()) {
        //
        // Sharded collections have the following further requirements for targeting:
        //
        // Limit-1 deletes must be targeted exactly by shard key *or* exact _id
        //

        // Get the shard key
        StatusWith<BSONObj> status =
            _routingInfo->cm()->getShardKeyPattern().extractShardKeyFromQuery(opCtx,
                                                                              deleteDoc.getQ());

        // Bad query
        if (!status.isOK())
            return status.getStatus();

        shardKey = status.getValue();
    }

    const auto collation = write_ops::collationOf(deleteDoc);

    // Target the shard key or delete query
    if (!shardKey.isEmpty()) {
        auto swEndpoint = _targetShardKey(shardKey, collation, 0);
        if (swEndpoint.isOK()) {
            return {{swEndpoint.getValue()}};
        }
    }

    // We failed to target a single shard.

    // Parse delete query.
    auto qr = std::make_unique<QueryRequest>(getNS());
    qr->setFilter(deleteDoc.getQ());
    if (!collation.isEmpty()) {
        qr->setCollation(collation);
    }
    const boost::intrusive_ptr<ExpressionContext> expCtx;
    auto cq = CanonicalQuery::canonicalize(opCtx,
                                           std::move(qr),
                                           expCtx,
                                           ExtensionsCallbackNoop(),
                                           MatchExpressionParser::kAllowAllSpecialFeatures);
    if (!cq.isOK()) {
        return cq.getStatus().withContext(str::stream()
                                          << "Could not parse delete query " << deleteDoc.getQ());
    }

    // Single deletes must target a single shard or be exact-ID.
    if (_routingInfo->cm() && !deleteDoc.getMulti() &&
        !isExactIdQuery(opCtx, *cq.getValue(), _routingInfo->cm().get())) {
        return Status(ErrorCodes::ShardKeyNotFound,
                      str::stream()
                          << "A single delete on a sharded collection must contain an exact "
                             "match on _id (and have the collection default collation) or "
                             "contain the shard key (and have the simple collation). Delete "
                             "request: "
                          << deleteDoc.toBSON() << ", shard key pattern: "
                          << _routingInfo->cm()->getShardKeyPattern().toString());
    }

    return _targetQuery(opCtx, deleteDoc.getQ(), collation);
}

StatusWith<std::vector<ShardEndpoint>> ChunkManagerTargeter::_targetQuery(
    OperationContext* opCtx, const BSONObj& query, const BSONObj& collation) const {
    if (!_routingInfo->db().primary() && !_routingInfo->cm()) {
        return {ErrorCodes::NamespaceNotFound,
                str::stream() << "could not target query in " << getNS().ns()
                              << "; no metadata found"};
    }

    if (!_routingInfo->cm()) {
        return std::vector<ShardEndpoint>{{_routingInfo->db().primaryId(),
                                           ChunkVersion::UNSHARDED(),
                                           _routingInfo->db().databaseVersion()}};
    }

    std::set<ShardId> shardIds;
    try {
        _routingInfo->cm()->getShardIdsForQuery(opCtx, query, collation, &shardIds);
    } catch (const DBException& ex) {
        return ex.toStatus();
    }

    std::vector<ShardEndpoint> endpoints;
    for (auto&& shardId : shardIds) {
        endpoints.emplace_back(std::move(shardId), _routingInfo->cm()->getVersion(shardId));
    }

    return endpoints;
}

StatusWith<ShardEndpoint> ChunkManagerTargeter::_targetShardKey(const BSONObj& shardKey,
                                                                const BSONObj& collation,
                                                                long long estDataSize) const {
    try {
        auto chunk = _routingInfo->cm()->findIntersectingChunk(shardKey, collation);
        return {{chunk.getShardId(), _routingInfo->cm()->getVersion(chunk.getShardId())}};
    } catch (const DBException& ex) {
        return ex.toStatus();
    }
    MONGO_UNREACHABLE;
}

StatusWith<std::vector<ShardEndpoint>> ChunkManagerTargeter::targetCollection() const {
    if (!_routingInfo->db().primary() && !_routingInfo->cm()) {
        return {ErrorCodes::NamespaceNotFound,
                str::stream() << "could not target full range of " << getNS().ns()
                              << "; metadata not found"};
    }

    if (!_routingInfo->cm()) {
        return std::vector<ShardEndpoint>{{_routingInfo->db().primaryId(),
                                           ChunkVersion::UNSHARDED(),
                                           _routingInfo->db().databaseVersion()}};
    }

    std::set<ShardId> shardIds;
    _routingInfo->cm()->getAllShardIds(&shardIds);

    std::vector<ShardEndpoint> endpoints;
    for (auto&& shardId : shardIds) {
        endpoints.emplace_back(std::move(shardId), _routingInfo->cm()->getVersion(shardId));
    }

    return endpoints;
}

StatusWith<std::vector<ShardEndpoint>> ChunkManagerTargeter::targetAllShards(
    OperationContext* opCtx) const {
    if (!_routingInfo->db().primary() && !_routingInfo->cm()) {
        return {ErrorCodes::NamespaceNotFound,
                str::stream() << "could not target every shard with versions for " << getNS().ns()
                              << "; metadata not found"};
    }

    std::vector<ShardId> shardIds;
    Grid::get(opCtx)->shardRegistry()->getAllShardIdsNoReload(&shardIds);

    // This function is only called if doing a multi write that targets more than one shard. This
    // implies the collection is sharded, so we should always have a chunk manager.
    invariant(_routingInfo->cm());

    std::vector<ShardEndpoint> endpoints;
    for (auto&& shardId : shardIds) {
        endpoints.emplace_back(std::move(shardId), _routingInfo->cm()->getVersion(shardId));
    }

    return endpoints;
}

void ChunkManagerTargeter::noteCouldNotTarget() {
    dassert(_remoteShardVersions.empty());
    dassert(!_remoteDbVersion);
    _needsTargetingRefresh = true;
}

void ChunkManagerTargeter::noteStaleShardResponse(const ShardEndpoint& endpoint,
                                                  const StaleConfigInfo& staleInfo) {
    dassert(!_needsTargetingRefresh);
    dassert(!_remoteDbVersion);

    ChunkVersion remoteShardVersion;
    if (!staleInfo.getVersionWanted()) {
        // If we don't have a vWanted sent, assume the version is higher than our current version.
        remoteShardVersion = getShardVersion(*_routingInfo, endpoint.shardName);
        remoteShardVersion.incMajor();
    } else {
        remoteShardVersion = *staleInfo.getVersionWanted();
    }

    ShardVersionMap::iterator it = _remoteShardVersions.find(endpoint.shardName);
    if (it == _remoteShardVersions.end()) {
        _remoteShardVersions.insert(std::make_pair(endpoint.shardName, remoteShardVersion));
    } else {
        ChunkVersion& previouslyNotedVersion = it->second;
        if (previouslyNotedVersion.epoch() == remoteShardVersion.epoch()) {
            if (previouslyNotedVersion.isOlderThan(remoteShardVersion)) {
                previouslyNotedVersion = remoteShardVersion;
            }
        } else {
            // Epoch changed midway while applying the batch so set the version to something
            // unique
            // and non-existent to force a reload when refreshIsNeeded is called.
            previouslyNotedVersion = ChunkVersion::IGNORED();
        }
    }
}

void ChunkManagerTargeter::noteStaleDbResponse(const ShardEndpoint& endpoint,
                                               const StaleDbRoutingVersion& staleInfo) {
    dassert(!_needsTargetingRefresh);
    dassert(_remoteShardVersions.empty());

    DatabaseVersion remoteDbVersion;
    if (!staleInfo.getVersionWanted()) {
        // If the vWanted is not set, assume the wanted version is higher than our current version.
        remoteDbVersion = _routingInfo->db().databaseVersion();
        remoteDbVersion = databaseVersion::makeIncremented(remoteDbVersion);
    } else {
        remoteDbVersion = *staleInfo.getVersionWanted();
    }

    // If databaseVersion was sent, only one shard should have been targeted. The shard should have
    // stopped processing the batch after one write encountered StaleDbVersion, after which the
    // shard should have simply copied that StaleDbVersion error as the error for the rest of the
    // writes in the batch. So, all of the write errors that contain a StaleDbVersion error should
    // contain the same vWanted version.
    if (_remoteDbVersion) {
        // Use uassert rather than invariant since this is asserting the contents of a network
        // response.
        uassert(
            ErrorCodes::InternalError,
            "Did not expect to get multiple StaleDbVersion errors with different vWanted versions",
            databaseVersion::equal(*_remoteDbVersion, remoteDbVersion));
        return;
    }
    _remoteDbVersion = remoteDbVersion;
}

Status ChunkManagerTargeter::refreshIfNeeded(OperationContext* opCtx, bool* wasChanged) {
    bool dummy;
    if (!wasChanged) {
        wasChanged = &dummy;
    }

    *wasChanged = false;

    LOG(4) << "ChunkManagerTargeter checking if refresh is needed, needsTargetingRefresh("
           << _needsTargetingRefresh << ") remoteShardVersions empty ("
           << _remoteShardVersions.empty() << ")"
           << ") remoteDbVersion empty (" << !_remoteDbVersion << ")";

    //
    // Did we have any stale config or targeting errors at all?
    //

    if (!_needsTargetingRefresh && _remoteShardVersions.empty() && !_remoteDbVersion) {
        return Status::OK();
    }

    //
    // Get the latest metadata information from the cache if there were issues
    //

    auto lastManager = _routingInfo->cm();
    auto lastDbVersion = _routingInfo->db().databaseVersion();

    auto initStatus = init(opCtx);
    if (!initStatus.isOK()) {
        return initStatus;
    }

    // We now have the latest metadata from the cache.

    //
    // See if and how we need to do a remote refresh.
    // Either we couldn't target at all, or we have stale versions, but not both.
    //

    if (_needsTargetingRefresh) {
        // Reset the field
        _needsTargetingRefresh = false;

        // If we couldn't target, we might need to refresh if we haven't remotely refreshed
        // the metadata since we last got it from the cache.

        bool alreadyRefreshed = wasMetadataRefreshed(
            lastManager, lastDbVersion, _routingInfo->cm(), _routingInfo->db().databaseVersion());

        // If didn't already refresh the targeting information, refresh it
        if (!alreadyRefreshed) {
            // To match previous behavior, we just need an incremental refresh here
            return _refreshShardVersionNow(opCtx);
        }

        *wasChanged = isMetadataDifferent(
            lastManager, lastDbVersion, _routingInfo->cm(), _routingInfo->db().databaseVersion());
        return Status::OK();
    } else if (!_remoteShardVersions.empty()) {
        // If we got stale shard versions from remote shards, we may need to refresh
        // NOTE: Not sure yet if this can happen simultaneously with targeting issues

        CompareResult result = compareAllShardVersions(*_routingInfo, _remoteShardVersions);

        LOG(4) << "ChunkManagerTargeter shard versions comparison result: " << (int)result;

        // Reset the versions
        _remoteShardVersions.clear();

        if (result == CompareResult_Unknown || result == CompareResult_LT) {
            // Our current shard versions aren't all comparable to the old versions, maybe drop
            return _refreshShardVersionNow(opCtx);
        }

        *wasChanged = isMetadataDifferent(
            lastManager, lastDbVersion, _routingInfo->cm(), _routingInfo->db().databaseVersion());
        return Status::OK();
    } else if (_remoteDbVersion) {
        // If we got stale database versions from the remote shard, we may need to refresh
        // NOTE: Not sure yet if this can happen simultaneously with targeting issues

        CompareResult result = compareDbVersions(*_routingInfo, *_remoteDbVersion);

        LOG(4) << "ChunkManagerTargeter database versions comparison result: " << (int)result;

        // Reset the version
        _remoteDbVersion = boost::none;

        if (result == CompareResult_Unknown || result == CompareResult_LT) {
            // Our current db version isn't always comparable to the old version, it may have been
            // dropped
            return _refreshDbVersionNow(opCtx);
        }

        *wasChanged = isMetadataDifferent(
            lastManager, lastDbVersion, _routingInfo->cm(), _routingInfo->db().databaseVersion());
        return Status::OK();
    }

    MONGO_UNREACHABLE;
}

Status ChunkManagerTargeter::_refreshShardVersionNow(OperationContext* opCtx) {
    Grid::get(opCtx)->catalogCache()->onStaleShardVersion(std::move(*_routingInfo));

    return init(opCtx);
}

Status ChunkManagerTargeter::_refreshDbVersionNow(OperationContext* opCtx) {
    Grid::get(opCtx)->catalogCache()->onStaleDatabaseVersion(
        _nss.db(), std::move(_routingInfo->db().databaseVersion()));

    return init(opCtx);
}

}  // namespace mongo
