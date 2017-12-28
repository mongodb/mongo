/**
 *    Copyright (C) 2013 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/s/commands/chunk_manager_targeter.h"

#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/collation/collation_index_key.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/commands/cluster_commands_helpers.h"
#include "mongo/s/grid.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace {

enum UpdateType { UpdateType_Replacement, UpdateType_OpStyle, UpdateType_Unknown };

enum CompareResult { CompareResult_Unknown, CompareResult_GTE, CompareResult_LT };

const ShardKeyPattern virtualIdShardKey(BSON("_id" << 1));

/**
 * There are two styles of update expressions:
 *
 * Replacement style: coll.update({ x : 1 }, { y : 2 })
 * OpStyle: coll.update({ x : 1 }, { $set : { y : 2 } })
 */
UpdateType getUpdateExprType(const BSONObj& updateExpr) {
    // Empty update is replacement-style, by default
    if (updateExpr.isEmpty()) {
        return UpdateType_Replacement;
    }

    UpdateType updateType = UpdateType_Unknown;

    BSONObjIterator it(updateExpr);
    while (it.more()) {
        BSONElement next = it.next();

        if (next.fieldName()[0] == '$') {
            if (updateType == UpdateType_Unknown) {
                updateType = UpdateType_OpStyle;
            } else if (updateType == UpdateType_Replacement) {
                return UpdateType_Unknown;
            }
        } else {
            if (updateType == UpdateType_Unknown) {
                updateType = UpdateType_Replacement;
            } else if (updateType == UpdateType_OpStyle) {
                return UpdateType_Unknown;
            }
        }
    }

    return updateType;
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
    auto shardKey = virtualIdShardKey.extractShardKeyFromQuery(query);
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
    if (!shardVersionA.hasEqualEpoch(shardVersionB)) {
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
        return !managerA->getVersion().isStrictlyEqualTo(managerB->getVersion());
    }

    dassert(NULL != primaryA.get());
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

ChunkManagerTargeter::ChunkManagerTargeter(const NamespaceString& nss, TargeterStats* stats)
    : _nss(nss), _needsTargetingRefresh(false), _stats(stats) {}


Status ChunkManagerTargeter::init(OperationContext* opCtx) {
    auto shardDbStatus = createShardDatabase(opCtx, _nss.db());
    if (!shardDbStatus.isOK()) {
        return shardDbStatus.getStatus();
    }

    const auto routingInfoStatus =
        Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfo(opCtx, _nss);
    if (!routingInfoStatus.isOK()) {
        return routingInfoStatus.getStatus();
    }

    _routingInfo = std::move(routingInfoStatus.getValue());

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
        if (!_routingInfo->primary()) {
            return Status(ErrorCodes::NamespaceNotFound,
                          str::stream() << "could not target insert in collection " << getNS().ns()
                                        << "; no metadata found");
        }

        return ShardEndpoint(_routingInfo->primary()->getId(), ChunkVersion::UNSHARDED());
    }

    return Status::OK();
}

StatusWith<std::vector<ShardEndpoint>> ChunkManagerTargeter::targetUpdate(
    OperationContext* opCtx, const write_ops::UpdateOpEntry& updateDoc) const {
    //
    // Update targeting may use either the query or the update.  This is to support save-style
    // updates, of the form:
    //
    // coll.update({ _id : xxx }, { _id : xxx, shardKey : 1, foo : bar }, { upsert : true })
    //
    // Because drivers do not know the shard key, they can't pull the shard key automatically
    // into the query doc, and to correctly support upsert we must target a single shard.
    //
    // The rule is simple - If the update is replacement style (no '$set'), we target using the
    // update.  If the update is not replacement style, we target using the query.
    //
    // If we have the exact shard key in either the query or replacement doc, we target using
    // that extracted key.
    //

    BSONObj query = updateDoc.getQ();
    BSONObj updateExpr = updateDoc.getU();

    UpdateType updateType = getUpdateExprType(updateExpr);

    if (updateType == UpdateType_Unknown) {
        return {ErrorCodes::UnsupportedFormat,
                str::stream() << "update document " << updateExpr
                              << " has mixed $operator and non-$operator style fields"};
    }

    BSONObj shardKey;

    if (_routingInfo->cm()) {
        //
        // Sharded collections have the following futher requirements for targeting:
        //
        // Upserts must be targeted exactly by shard key.
        // Non-multi updates must be targeted exactly by shard key *or* exact _id.
        //

        // Get the shard key
        if (updateType == UpdateType_OpStyle) {
            // Target using the query
            StatusWith<BSONObj> status =
                _routingInfo->cm()->getShardKeyPattern().extractShardKeyFromQuery(opCtx, query);

            // Bad query
            if (!status.isOK())
                return status.getStatus();

            shardKey = status.getValue();
        } else {
            // Target using the replacement document
            shardKey = _routingInfo->cm()->getShardKeyPattern().extractShardKeyFromDoc(updateExpr);
        }

        // Check shard key size on upsert.
        if (updateDoc.getUpsert()) {
            Status status = ShardKeyPattern::checkShardKeySize(shardKey);
            if (!status.isOK())
                return status;
        }
    }

    const auto collation = write_ops::collationOf(updateDoc);

    // Target the shard key, query, or replacement doc
    if (!shardKey.isEmpty()) {
        try {
            return std::vector<ShardEndpoint>{
                _targetShardKey(shardKey, collation, (query.objsize() + updateExpr.objsize()))};
        } catch (const DBException&) {
            // This update is potentially not constrained to a single shard
        }
    }

    // We failed to target a single shard.

    // Upserts are required to target a single shard.
    if (_routingInfo->cm() && updateDoc.getUpsert()) {
        return Status(ErrorCodes::ShardKeyNotFound,
                      str::stream() << "An upsert on a sharded collection must contain the shard "
                                       "key and have the simple collation. Update request: "
                                    << updateDoc.toBSON()
                                    << ", shard key pattern: "
                                    << _routingInfo->cm()->getShardKeyPattern().toString());
    }

    // Parse update query.
    auto qr = stdx::make_unique<QueryRequest>(getNS());
    qr->setFilter(updateDoc.getQ());
    if (!collation.isEmpty()) {
        qr->setCollation(collation);
    }
    // $expr is not allowed in the query for an upsert, since it is not clear what the equality
    // extraction behavior for $expr should be.
    auto allowedMatcherFeatures = MatchExpressionParser::kAllowAllSpecialFeatures;
    if (updateDoc.getUpsert()) {
        allowedMatcherFeatures &= ~MatchExpressionParser::AllowedFeatures::kExpr;
    }
    const boost::intrusive_ptr<ExpressionContext> expCtx;
    auto cq = CanonicalQuery::canonicalize(
        opCtx, std::move(qr), expCtx, ExtensionsCallbackNoop(), allowedMatcherFeatures);
    if (!cq.isOK() && cq.getStatus().code() == ErrorCodes::QueryFeatureNotAllowed) {
        // The default error message for disallowed $expr is not descriptive enough, so we rewrite
        // it here.
        return {ErrorCodes::QueryFeatureNotAllowed,
                "$expr is not allowed in the query predicate for an upsert"};
    }
    if (!cq.isOK()) {
        return {cq.getStatus().code(),
                str::stream() << "Could not parse update query " << updateDoc.getQ()
                              << causedBy(cq.getStatus())};
    }

    // Single (non-multi) updates must target a single shard or be exact-ID.
    if (_routingInfo->cm() && !updateDoc.getMulti() &&
        !isExactIdQuery(opCtx, *cq.getValue(), _routingInfo->cm().get())) {
        return {ErrorCodes::ShardKeyNotFound,
                str::stream() << "A single update on a sharded collection must contain an exact "
                                 "match on _id (and have the collection default collation) or "
                                 "contain the shard key (and have the simple collation). Update "
                                 "request: "
                              << updateDoc.toBSON()
                              << ", shard key pattern: "
                              << _routingInfo->cm()->getShardKeyPattern().toString()};
    }

    if (updateType == UpdateType_OpStyle) {
        return _targetQuery(opCtx, query, collation);
    } else {
        return _targetDoc(opCtx, updateExpr, collation);
    }
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
        try {
            return std::vector<ShardEndpoint>{_targetShardKey(shardKey, collation, 0)};
        } catch (const DBException&) {
            // This delete is potentially not constrained to a single shard
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
        return Status(cq.getStatus().code(),
                      str::stream() << "Could not parse delete query " << deleteDoc.getQ()
                                    << causedBy(cq.getStatus()));
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
                          << deleteDoc.toBSON()
                          << ", shard key pattern: "
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
    if (!_routingInfo->primary() && !_routingInfo->cm()) {
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
        shardIds.insert(_routingInfo->primary()->getId());
    }

    std::vector<ShardEndpoint> endpoints;
    for (auto&& shardId : shardIds) {
        const auto version = _routingInfo->cm() ? _routingInfo->cm()->getVersion(shardId)
                                                : ChunkVersion::UNSHARDED();
        endpoints.emplace_back(std::move(shardId), version);
    }

    return endpoints;
}

ShardEndpoint ChunkManagerTargeter::_targetShardKey(const BSONObj& shardKey,
                                                    const BSONObj& collation,
                                                    long long estDataSize) const {
    const auto chunk = _routingInfo->cm()->findIntersectingChunk(shardKey, collation);

    // Track autosplit stats for sharded collections
    // Note: this is only best effort accounting and is not accurate.
    if (estDataSize > 0) {
        _stats->chunkSizeDelta[chunk->getMin()] += estDataSize;
    }

    return {chunk->getShardId(), _routingInfo->cm()->getVersion(chunk->getShardId())};
}

StatusWith<std::vector<ShardEndpoint>> ChunkManagerTargeter::targetCollection() const {
    if (!_routingInfo->primary() && !_routingInfo->cm()) {
        return {ErrorCodes::NamespaceNotFound,
                str::stream() << "could not target full range of " << getNS().ns()
                              << "; metadata not found"};
    }

    std::set<ShardId> shardIds;
    if (_routingInfo->cm()) {
        _routingInfo->cm()->getAllShardIds(&shardIds);
    } else {
        shardIds.insert(_routingInfo->primary()->getId());
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
    if (!_routingInfo->primary() && !_routingInfo->cm()) {
        return {ErrorCodes::NamespaceNotFound,
                str::stream() << "could not target every shard with versions for " << getNS().ns()
                              << "; metadata not found"};
    }

    std::vector<ShardId> shardIds;
    Grid::get(opCtx)->shardRegistry()->getAllShardIds(&shardIds);

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
                                             const BSONObj& staleInfo) {
    dassert(!_needsTargetingRefresh);

    ChunkVersion remoteShardVersion;
    if (staleInfo["vWanted"].eoo()) {
        // If we don't have a vWanted sent, assume the version is higher than our current
        // version.
        remoteShardVersion = getShardVersion(*_routingInfo, endpoint.shardName);
        remoteShardVersion.incMajor();
    } else {
        remoteShardVersion = ChunkVersion::fromBSON(staleInfo, "vWanted");
    }

    ShardVersionMap::iterator it = _remoteShardVersions.find(endpoint.shardName);
    if (it == _remoteShardVersions.end()) {
        _remoteShardVersions.insert(std::make_pair(endpoint.shardName, remoteShardVersion));
    } else {
        ChunkVersion& previouslyNotedVersion = it->second;
        if (previouslyNotedVersion.hasEqualEpoch(remoteShardVersion)) {
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
    auto lastPrimary = _routingInfo->primary();

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
        // the
        // metadata since we last got it from the cache.

        bool alreadyRefreshed = wasMetadataRefreshed(
            lastManager, lastPrimary, _routingInfo->cm(), _routingInfo->primary());

        // If didn't already refresh the targeting information, refresh it
        if (!alreadyRefreshed) {
            // To match previous behavior, we just need an incremental refresh here
            return _refreshNow(opCtx);
        }

        *wasChanged = isMetadataDifferent(
            lastManager, lastPrimary, _routingInfo->cm(), _routingInfo->primary());
        return Status::OK();
    } else if (!_remoteShardVersions.empty()) {
        // If we got stale shard versions from remote shards, we may need to refresh
        // NOTE: Not sure yet if this can happen simultaneously with targeting issues

        CompareResult result = compareAllShardVersions(*_routingInfo, _remoteShardVersions);

        // Reset the versions
        _remoteShardVersions.clear();

        if (result == CompareResult_Unknown || result == CompareResult_LT) {
            // Our current shard versions aren't all comparable to the old versions, maybe drop
            return _refreshNow(opCtx);
        }

        *wasChanged = isMetadataDifferent(
            lastManager, lastPrimary, _routingInfo->cm(), _routingInfo->primary());
        return Status::OK();
    }

    MONGO_UNREACHABLE;
}

Status ChunkManagerTargeter::_refreshNow(OperationContext* opCtx) {
    Grid::get(opCtx)->catalogCache()->onStaleConfigError(std::move(*_routingInfo));

    return init(opCtx);
}

}  // namespace mongo
