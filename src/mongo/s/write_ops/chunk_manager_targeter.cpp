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
StatusWith<BSONObj> getUpdateExpr(OperationContext* opCtx,
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

    // Find the set of all shard key fields that are missing from the update expression.
    const auto missingFields = shardKeyPattern.findMissingShardKeyFieldsFromDoc(updateExpr);

    // If there are no missing fields, return the update expression as-is.
    if (missingFields.empty()) {
        return updateExpr;
    }
    // If there are any missing fields other than _id, then this update can never succeed.
    if (missingFields.size() > 1 || *missingFields.begin() != kIdFieldName) {
        return {ErrorCodes::ShardKeyNotFound,
                str::stream() << "Expected replacement document to include all shard key fields, "
                                 "but the following were omitted: "
                              << BSON("missingShardKeyFields" << missingFields)};
    }
    // If the only missing field is _id, attempt to extract it from an exact match in the update's
    // query spec. This will guarantee that we can target a single shard, but it is not necessarily
    // fatal if no exact _id can be found.
    invariant(missingFields.size() == 1 && *missingFields.begin() == kIdFieldName);
    const auto idFromQuery = kVirtualIdShardKey.extractShardKeyFromQuery(opCtx, updateDoc.getQ());
    if (!idFromQuery.isOK()) {
        return idFromQuery;
    } else if (auto idElt = idFromQuery.getValue()[kIdFieldName]) {
        updateExpr = updateExpr.addField(idElt);
    }
    // Confirm that the finalized replacement shard key is valid.
    auto skStatus =
        ShardKeyPattern::checkShardKeySize(shardKeyPattern.extractShardKeyFromDoc(updateExpr));
    if (!skStatus.isOK()) {
        return skStatus;
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
    auto qr = stdx::make_unique<QueryRequest>(nss);
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
// Utilities to compare shard versions
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

/**
 * Whether or not the manager/primary pair is different from the other manager/primary pair.
 */
bool isMetadataDifferent(const std::shared_ptr<ChunkManager>& managerA,
                         const std::shared_ptr<Shard>& primaryA,
                         const std::shared_ptr<ChunkManager>& managerB,
                         const std::shared_ptr<Shard>& primaryB) {
    if ((managerA && !managerB) || (!managerA && managerB) || (primaryA && !primaryB) ||
        (!primaryA && primaryB))
        return true;

    if (managerA) {
        return managerA->getVersion() != managerB->getVersion();
    }

    return primaryA->getId() != primaryB->getId();
}

/**
 * Whether or not the manager/primary pair was changed or refreshed from a previous version
 * of the metadata.
 */
bool wasMetadataRefreshed(const std::shared_ptr<ChunkManager>& managerA,
                          const std::shared_ptr<Shard>& primaryA,
                          const std::shared_ptr<ChunkManager>& managerB,
                          const std::shared_ptr<Shard>& primaryB) {
    if (isMetadataDifferent(managerA, primaryA, managerB, primaryB))
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
        //
        // Sharded collections have the following requirements for targeting:
        //
        // Inserts must contain the exact shard key.
        //

        shardKey = _routingInfo->cm()->getShardKeyPattern().extractShardKeyFromDoc(doc);

        // Check shard key exists
        if (shardKey.isEmpty()) {
            return {ErrorCodes::ShardKeyNotFound,
                    str::stream() << "document " << doc
                                  << " does not contain shard key for pattern "
                                  << _routingInfo->cm()->getShardKeyPattern().toString()};
        }

        // Check shard key size on insert
        Status status = ShardKeyPattern::checkShardKeySize(shardKey);
        if (!status.isOK())
            return status;
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

        return ShardEndpoint(_routingInfo->db().primary()->getId(), ChunkVersion::UNSHARDED());
    }

    return Status::OK();
}

StatusWith<std::vector<ShardEndpoint>> ChunkManagerTargeter::targetUpdate(
    OperationContext* opCtx, const write_ops::UpdateOpEntry& updateDoc) const {
    //
    // The rule is simple; we always attempt to target using the query first. If the update is
    // replacement-style and the query targets more than one shard, we fall back to targeting using
    // the replacement document, since it must always contain a full shard key. Upserts should have
    // full shard key in the query and we always use query for targeting. Because mongoD will
    // automatically propagate '_id' from an existing document, and will extract it from an
    // exact-match in the query in the case of an upsert, we augment the replacement doc with the
    // query's '_id' for targeting purposes, if it exists.
    //
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
        return std::vector<ShardEndpoint>{
            {_routingInfo->db().primaryId(), ChunkVersion::UNSHARDED()}};
    }

    const auto& shardKeyPattern = _routingInfo->cm()->getShardKeyPattern();
    const auto collation = write_ops::collationOf(updateDoc);

    const auto updateExpr = getUpdateExpr(opCtx, shardKeyPattern, updateType.getValue(), updateDoc);
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
    auto qr = stdx::make_unique<QueryRequest>(getNS());
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

StatusWith<std::vector<ShardEndpoint>> ChunkManagerTargeter::_targetDoc(
    OperationContext* opCtx, const BSONObj& doc, const BSONObj& collation) const {
    // NOTE: This is weird and fragile, but it's the way our language works right now - documents
    // are either A) invalid or B) valid equality queries over themselves.
    return _targetQuery(opCtx, doc, collation);
}

StatusWith<std::vector<ShardEndpoint>> ChunkManagerTargeter::_targetQuery(
    OperationContext* opCtx, const BSONObj& query, const BSONObj& collation) const {
    if (!_routingInfo->db().primary() && !_routingInfo->cm()) {
        return {ErrorCodes::NamespaceNotFound,
                str::stream() << "could not target query in " << getNS().ns()
                              << "; no metadata found"};
    }

    std::set<ShardId> shardIds;
    if (_routingInfo->cm()) {
        try {
            _routingInfo->cm()->getShardIdsForQuery(opCtx, query, collation, &shardIds);
        } catch (const DBException& ex) {
            return ex.toStatus();
        }
    } else {
        shardIds.insert(_routingInfo->db().primary()->getId());
    }

    std::vector<ShardEndpoint> endpoints;
    for (auto&& shardId : shardIds) {
        const auto version = _routingInfo->cm() ? _routingInfo->cm()->getVersion(shardId)
                                                : ChunkVersion::UNSHARDED();
        endpoints.emplace_back(std::move(shardId), version);
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

    std::set<ShardId> shardIds;
    if (_routingInfo->cm()) {
        _routingInfo->cm()->getAllShardIds(&shardIds);
    } else {
        shardIds.insert(_routingInfo->db().primary()->getId());
    }

    std::vector<ShardEndpoint> endpoints;
    for (auto&& shardId : shardIds) {
        const auto version = _routingInfo->cm() ? _routingInfo->cm()->getVersion(shardId)
                                                : ChunkVersion::UNSHARDED();
        endpoints.emplace_back(std::move(shardId), version);
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

    std::vector<ShardEndpoint> endpoints;
    for (auto&& shardId : shardIds) {
        const auto version = _routingInfo->cm() ? _routingInfo->cm()->getVersion(shardId)
                                                : ChunkVersion::UNSHARDED();
        endpoints.emplace_back(std::move(shardId), version);
    }

    return endpoints;
}

void ChunkManagerTargeter::noteCouldNotTarget() {
    dassert(_remoteShardVersions.empty());
    _needsTargetingRefresh = true;
}

void ChunkManagerTargeter::noteStaleResponse(const ShardEndpoint& endpoint,
                                             const StaleConfigInfo& staleInfo) {
    dassert(!_needsTargetingRefresh);

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

Status ChunkManagerTargeter::refreshIfNeeded(OperationContext* opCtx, bool* wasChanged) {
    bool dummy;
    if (!wasChanged) {
        wasChanged = &dummy;
    }

    *wasChanged = false;

    LOG(4) << "ChunkManagerTargeter checking if refresh is needed, needsTargetingRefresh("
           << _needsTargetingRefresh << ") remoteShardVersions empty ("
           << _remoteShardVersions.empty() << ")";

    //
    // Did we have any stale config or targeting errors at all?
    //

    if (!_needsTargetingRefresh && _remoteShardVersions.empty()) {
        return Status::OK();
    }

    //
    // Get the latest metadata information from the cache if there were issues
    //

    auto lastManager = _routingInfo->cm();
    auto lastPrimary = _routingInfo->db().primary();

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
        _remoteShardVersions.clear();
        _needsTargetingRefresh = false;

        // If we couldn't target, we might need to refresh if we haven't remotely refreshed
        // the
        // metadata since we last got it from the cache.

        bool alreadyRefreshed = wasMetadataRefreshed(
            lastManager, lastPrimary, _routingInfo->cm(), _routingInfo->db().primary());

        // If didn't already refresh the targeting information, refresh it
        if (!alreadyRefreshed) {
            // To match previous behavior, we just need an incremental refresh here
            return _refreshNow(opCtx);
        }

        *wasChanged = isMetadataDifferent(
            lastManager, lastPrimary, _routingInfo->cm(), _routingInfo->db().primary());
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
            return _refreshNow(opCtx);
        }

        *wasChanged = isMetadataDifferent(
            lastManager, lastPrimary, _routingInfo->cm(), _routingInfo->db().primary());
        return Status::OK();
    }

    MONGO_UNREACHABLE;
}

int ChunkManagerTargeter::getNShardsOwningChunks() const {
    if (_routingInfo->cm()) {
        return _routingInfo->cm()->getNShardsOwningChunks();
    }

    return 0;
}

Status ChunkManagerTargeter::_refreshNow(OperationContext* opCtx) {
    Grid::get(opCtx)->catalogCache()->onStaleShardVersion(std::move(*_routingInfo));

    return init(opCtx);
}

}  // namespace mongo
