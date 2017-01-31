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

#include "mongo/s/chunk_manager_targeter.h"

#include <boost/thread/tss.hpp>

#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/collation/collation_index_key.h"
#include "mongo/s/catalog/catalog_cache.h"
#include "mongo/s/chunk.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/s/sharding_raii.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

using std::shared_ptr;
using str::stream;
using std::map;
using std::set;
using std::string;
using std::vector;

namespace {

enum UpdateType { UpdateType_Replacement, UpdateType_OpStyle, UpdateType_Unknown };

enum CompareResult { CompareResult_Unknown, CompareResult_GTE, CompareResult_LT };

const ShardKeyPattern virtualIdShardKey(BSON("_id" << 1));

// To match legacy reload behavior, we have to backoff on config reload per-thread
// TODO: Centralize this behavior better by refactoring config reload in mongos
boost::thread_specific_ptr<Backoff> perThreadBackoff;
const int maxWaitMillis = 500;

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
bool isExactIdQuery(OperationContext* txn, const CanonicalQuery& query, ChunkManager* manager) {
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

void refreshBackoff() {
    if (!perThreadBackoff.get()) {
        perThreadBackoff.reset(new Backoff(maxWaitMillis, maxWaitMillis * 2));
    }

    perThreadBackoff.get()->nextSleepMillis();
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

    if (shardVersionA < shardVersionB) {
        return CompareResult_LT;
    }

    else
        return CompareResult_GTE;
}

ChunkVersion getShardVersion(StringData shardName,
                             const ChunkManager* manager,
                             const Shard* primary) {
    dassert(!(manager && primary));
    dassert(manager || primary);

    if (primary) {
        return ChunkVersion::UNSHARDED();
    }

    return manager->getVersion(shardName.toString());
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
CompareResult compareAllShardVersions(const ChunkManager* cachedChunkManager,
                                      const Shard* cachedPrimary,
                                      const map<ShardId, ChunkVersion>& remoteShardVersions) {
    CompareResult finalResult = CompareResult_GTE;

    for (map<ShardId, ChunkVersion>::const_iterator it = remoteShardVersions.begin();
         it != remoteShardVersions.end();
         ++it) {
        // Get the remote and cached version for the next shard
        const ShardId& shardName = it->first;
        const ChunkVersion& remoteShardVersion = it->second;

        ChunkVersion cachedShardVersion;

        try {
            // Throws b/c shard constructor throws
            cachedShardVersion =
                getShardVersion(shardName.toString(), cachedChunkManager, cachedPrimary);
        } catch (const DBException& ex) {
            warning() << "could not lookup shard " << shardName
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
bool isMetadataDifferent(const shared_ptr<ChunkManager>& managerA,
                         const shared_ptr<Shard>& primaryA,
                         const shared_ptr<ChunkManager>& managerB,
                         const shared_ptr<Shard>& primaryB) {
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
bool wasMetadataRefreshed(const shared_ptr<ChunkManager>& managerA,
                          const shared_ptr<Shard>& primaryA,
                          const shared_ptr<ChunkManager>& managerB,
                          const shared_ptr<Shard>& primaryB) {
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


Status ChunkManagerTargeter::init(OperationContext* txn) {
    auto scopedCMStatus = ScopedChunkManager::getOrCreate(txn, _nss);
    if (!scopedCMStatus.isOK()) {
        return scopedCMStatus.getStatus();
    }

    const auto& scopedCM = scopedCMStatus.getValue();
    _manager = scopedCM.cm();
    _primary = scopedCM.primary();

    return Status::OK();
}

const NamespaceString& ChunkManagerTargeter::getNS() const {
    return _nss;
}

Status ChunkManagerTargeter::targetInsert(OperationContext* txn,
                                          const BSONObj& doc,
                                          ShardEndpoint** endpoint) const {
    BSONObj shardKey;

    if (_manager) {
        //
        // Sharded collections have the following requirements for targeting:
        //
        // Inserts must contain the exact shard key.
        //

        shardKey = _manager->getShardKeyPattern().extractShardKeyFromDoc(doc);

        // Check shard key exists
        if (shardKey.isEmpty()) {
            return Status(ErrorCodes::ShardKeyNotFound,
                          stream() << "document " << doc
                                   << " does not contain shard key for pattern "
                                   << _manager->getShardKeyPattern().toString());
        }

        // Check shard key size on insert
        Status status = ShardKeyPattern::checkShardKeySize(shardKey);
        if (!status.isOK())
            return status;
    }

    // Target the shard key or database primary
    if (!shardKey.isEmpty()) {
        return targetShardKey(txn, shardKey, CollationSpec::kSimpleSpec, doc.objsize(), endpoint);
    } else {
        if (!_primary) {
            return Status(ErrorCodes::NamespaceNotFound,
                          str::stream() << "could not target insert in collection " << getNS().ns()
                                        << "; no metadata found");
        }

        *endpoint = new ShardEndpoint(_primary->getId(), ChunkVersion::UNSHARDED());
        return Status::OK();
    }
}

Status ChunkManagerTargeter::targetUpdate(OperationContext* txn,
                                          const BatchedUpdateDocument& updateDoc,
                                          vector<ShardEndpoint*>* endpoints) const {
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
    // update.  If the update is replacement style, we target using the query.
    //
    // If we have the exact shard key in either the query or replacement doc, we target using
    // that extracted key.
    //

    BSONObj query = updateDoc.getQuery();
    BSONObj updateExpr = updateDoc.getUpdateExpr();

    UpdateType updateType = getUpdateExprType(updateDoc.getUpdateExpr());

    if (updateType == UpdateType_Unknown) {
        return Status(ErrorCodes::UnsupportedFormat,
                      stream() << "update document " << updateExpr
                               << " has mixed $operator and non-$operator style fields");
    }

    BSONObj shardKey;

    if (_manager) {
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
                _manager->getShardKeyPattern().extractShardKeyFromQuery(txn, query);

            // Bad query
            if (!status.isOK())
                return status.getStatus();

            shardKey = status.getValue();
        } else {
            // Target using the replacement document
            shardKey = _manager->getShardKeyPattern().extractShardKeyFromDoc(updateExpr);
        }

        // Check shard key size on upsert.
        if (updateDoc.getUpsert()) {
            Status status = ShardKeyPattern::checkShardKeySize(shardKey);
            if (!status.isOK())
                return status;
        }
    }

    const BSONObj collation = updateDoc.isCollationSet() ? updateDoc.getCollation() : BSONObj();

    // Target the shard key, query, or replacement doc
    if (!shardKey.isEmpty()) {
        // We can't rely on our query targeting to be exact
        ShardEndpoint* endpoint = NULL;
        Status result = targetShardKey(
            txn, shardKey, collation, (query.objsize() + updateExpr.objsize()), &endpoint);
        if (result.isOK()) {
            endpoints->push_back(endpoint);
            return result;
        }
    }

    // We failed to target a single shard.

    // Upserts are required to target a single shard.
    if (_manager && updateDoc.getUpsert()) {
        return Status(ErrorCodes::ShardKeyNotFound,
                      str::stream() << "An upsert on a sharded collection must contain the shard "
                                       "key and have the simple collation. Update request: "
                                    << updateDoc.toBSON()
                                    << ", shard key pattern: "
                                    << _manager->getShardKeyPattern().toString());
    }

    // Parse update query.
    auto qr = stdx::make_unique<QueryRequest>(getNS());
    qr->setFilter(updateDoc.getQuery());
    if (!collation.isEmpty()) {
        qr->setCollation(collation);
    }
    auto cq = CanonicalQuery::canonicalize(txn, std::move(qr), ExtensionsCallbackNoop());
    if (!cq.isOK()) {
        return Status(cq.getStatus().code(),
                      str::stream() << "Could not parse update query " << updateDoc.getQuery()
                                    << causedBy(cq.getStatus()));
    }

    // Single (non-multi) updates must target a single shard or be exact-ID.
    if (_manager && !updateDoc.getMulti() && !isExactIdQuery(txn, *cq.getValue(), _manager.get())) {
        return Status(ErrorCodes::ShardKeyNotFound,
                      str::stream()
                          << "A single update on a sharded collection must contain an exact "
                             "match on _id (and have the collection default collation) or "
                             "contain the shard key (and have the simple collation). Update "
                             "request: "
                          << updateDoc.toBSON()
                          << ", shard key pattern: "
                          << _manager->getShardKeyPattern().toString());
    }

    if (updateType == UpdateType_OpStyle) {
        return targetQuery(txn, query, collation, endpoints);
    } else {
        return targetDoc(txn, updateExpr, collation, endpoints);
    }
}

Status ChunkManagerTargeter::targetDelete(OperationContext* txn,
                                          const BatchedDeleteDocument& deleteDoc,
                                          vector<ShardEndpoint*>* endpoints) const {
    BSONObj shardKey;

    if (_manager) {
        //
        // Sharded collections have the following further requirements for targeting:
        //
        // Limit-1 deletes must be targeted exactly by shard key *or* exact _id
        //

        // Get the shard key
        StatusWith<BSONObj> status =
            _manager->getShardKeyPattern().extractShardKeyFromQuery(txn, deleteDoc.getQuery());

        // Bad query
        if (!status.isOK())
            return status.getStatus();

        shardKey = status.getValue();
    }

    const BSONObj collation = deleteDoc.isCollationSet() ? deleteDoc.getCollation() : BSONObj();


    // Target the shard key or delete query
    if (!shardKey.isEmpty()) {
        // We can't rely on our query targeting to be exact
        ShardEndpoint* endpoint = NULL;
        Status result = targetShardKey(txn, shardKey, collation, 0, &endpoint);
        if (result.isOK()) {
            endpoints->push_back(endpoint);
            return result;
        }
    }

    // We failed to target a single shard.

    // Parse delete query.
    auto qr = stdx::make_unique<QueryRequest>(getNS());
    qr->setFilter(deleteDoc.getQuery());
    if (!collation.isEmpty()) {
        qr->setCollation(collation);
    }
    auto cq = CanonicalQuery::canonicalize(txn, std::move(qr), ExtensionsCallbackNoop());
    if (!cq.isOK()) {
        return Status(cq.getStatus().code(),
                      str::stream() << "Could not parse delete query " << deleteDoc.getQuery()
                                    << causedBy(cq.getStatus()));
    }

    // Single deletes must target a single shard or be exact-ID.
    if (_manager && deleteDoc.getLimit() == 1 &&
        !isExactIdQuery(txn, *cq.getValue(), _manager.get())) {
        return Status(ErrorCodes::ShardKeyNotFound,
                      str::stream()
                          << "A single delete on a sharded collection must contain an exact "
                             "match on _id (and have the collection default collation) or "
                             "contain the shard key (and have the simple collation). Delete "
                             "request: "
                          << deleteDoc.toBSON()
                          << ", shard key pattern: "
                          << _manager->getShardKeyPattern().toString());
    }

    return targetQuery(txn, deleteDoc.getQuery(), collation, endpoints);
}

Status ChunkManagerTargeter::targetDoc(OperationContext* txn,
                                       const BSONObj& doc,
                                       const BSONObj& collation,
                                       vector<ShardEndpoint*>* endpoints) const {
    // NOTE: This is weird and fragile, but it's the way our language works right now -
    // documents are either A) invalid or B) valid equality queries over themselves.
    return targetQuery(txn, doc, collation, endpoints);
}

Status ChunkManagerTargeter::targetQuery(OperationContext* txn,
                                         const BSONObj& query,
                                         const BSONObj& collation,
                                         vector<ShardEndpoint*>* endpoints) const {
    if (!_primary && !_manager) {
        return Status(ErrorCodes::NamespaceNotFound,
                      stream() << "could not target query in " << getNS().ns()
                               << "; no metadata found");
    }

    set<ShardId> shardIds;
    if (_manager) {
        try {
            _manager->getShardIdsForQuery(txn, query, collation, &shardIds);
        } catch (const DBException& ex) {
            return ex.toStatus();
        }
    } else {
        shardIds.insert(_primary->getId());
    }

    for (const ShardId& shardId : shardIds) {
        endpoints->push_back(new ShardEndpoint(
            shardId, _manager ? _manager->getVersion(shardId) : ChunkVersion::UNSHARDED()));
    }

    return Status::OK();
}

Status ChunkManagerTargeter::targetShardKey(OperationContext* txn,
                                            const BSONObj& shardKey,
                                            const BSONObj& collation,
                                            long long estDataSize,
                                            ShardEndpoint** endpoint) const {
    invariant(NULL != _manager);

    auto chunk = _manager->findIntersectingChunk(txn, shardKey, collation);
    if (!chunk.isOK()) {
        return chunk.getStatus();
    }

    // Track autosplit stats for sharded collections
    // Note: this is only best effort accounting and is not accurate.
    if (estDataSize > 0) {
        _stats->chunkSizeDelta[chunk.getValue()->getMin()] += estDataSize;
    }

    *endpoint = new ShardEndpoint(chunk.getValue()->getShardId(),
                                  _manager->getVersion(chunk.getValue()->getShardId()));

    return Status::OK();
}

Status ChunkManagerTargeter::targetCollection(vector<ShardEndpoint*>* endpoints) const {
    if (!_primary && !_manager) {
        return Status(ErrorCodes::NamespaceNotFound,
                      str::stream() << "could not target full range of " << getNS().ns()
                                    << "; metadata not found");
    }

    set<ShardId> shardIds;
    if (_manager) {
        _manager->getAllShardIds(&shardIds);
    } else {
        shardIds.insert(_primary->getId());
    }

    for (const ShardId& shardId : shardIds) {
        endpoints->push_back(new ShardEndpoint(
            shardId, _manager ? _manager->getVersion(shardId) : ChunkVersion::UNSHARDED()));
    }

    return Status::OK();
}

Status ChunkManagerTargeter::targetAllShards(vector<ShardEndpoint*>* endpoints) const {
    if (!_primary && !_manager) {
        return Status(ErrorCodes::NamespaceNotFound,
                      str::stream() << "could not target every shard with versions for "
                                    << getNS().ns()
                                    << "; metadata not found");
    }

    vector<ShardId> shardIds;
    grid.shardRegistry()->getAllShardIds(&shardIds);

    for (const ShardId& shardId : shardIds) {
        endpoints->push_back(new ShardEndpoint(
            shardId, _manager ? _manager->getVersion(shardId) : ChunkVersion::UNSHARDED()));
    }

    return Status::OK();
}

void ChunkManagerTargeter::noteStaleResponse(const ShardEndpoint& endpoint,
                                             const BSONObj& staleInfo) {
    dassert(!_needsTargetingRefresh);

    ChunkVersion remoteShardVersion;
    if (staleInfo["vWanted"].eoo()) {
        // If we don't have a vWanted sent, assume the version is higher than our current
        // version.
        remoteShardVersion =
            getShardVersion(endpoint.shardName.toString(), _manager.get(), _primary.get());
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
            // Epoch changed midway while applying the batch so set the version to something unique
            // and non-existent to force a reload when refreshIsNeeded is called.
            previouslyNotedVersion = ChunkVersion::IGNORED();
        }
    }
}

void ChunkManagerTargeter::noteCouldNotTarget() {
    dassert(_remoteShardVersions.empty());
    _needsTargetingRefresh = true;
}

Status ChunkManagerTargeter::refreshIfNeeded(OperationContext* txn, bool* wasChanged) {
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

    shared_ptr<ChunkManager> lastManager = _manager;
    shared_ptr<Shard> lastPrimary = _primary;

    auto scopedCMStatus = ScopedChunkManager::getOrCreate(txn, _nss);
    if (!scopedCMStatus.isOK()) {
        return scopedCMStatus.getStatus();
    }

    const auto& scopedCM = scopedCMStatus.getValue();
    _manager = scopedCM.cm();
    _primary = scopedCM.primary();

    // We now have the latest metadata from the cache.

    //
    // See if and how we need to do a remote refresh.
    // Either we couldn't target at all, or we have stale versions, but not both.
    //

    dassert(!(_needsTargetingRefresh && !_remoteShardVersions.empty()));

    if (_needsTargetingRefresh) {
        // Reset the field
        _needsTargetingRefresh = false;

        // If we couldn't target, we might need to refresh if we haven't remotely refreshed the
        // metadata since we last got it from the cache.

        bool alreadyRefreshed = wasMetadataRefreshed(lastManager, lastPrimary, _manager, _primary);

        // If didn't already refresh the targeting information, refresh it
        if (!alreadyRefreshed) {
            // To match previous behavior, we just need an incremental refresh here
            return refreshNow(txn, RefreshType_RefreshChunkManager);
        }

        *wasChanged = isMetadataDifferent(lastManager, lastPrimary, _manager, _primary);
        return Status::OK();
    } else if (!_remoteShardVersions.empty()) {
        // If we got stale shard versions from remote shards, we may need to refresh
        // NOTE: Not sure yet if this can happen simultaneously with targeting issues

        CompareResult result =
            compareAllShardVersions(_manager.get(), _primary.get(), _remoteShardVersions);
        // Reset the versions
        _remoteShardVersions.clear();

        if (result == CompareResult_Unknown) {
            // Our current shard versions aren't all comparable to the old versions, maybe drop
            return refreshNow(txn, RefreshType_ReloadDatabase);
        } else if (result == CompareResult_LT) {
            // Our current shard versions are less than the remote versions, but no drop
            return refreshNow(txn, RefreshType_RefreshChunkManager);
        }

        *wasChanged = isMetadataDifferent(lastManager, lastPrimary, _manager, _primary);
        return Status::OK();
    }

    // unreachable
    dassert(false);
    return Status::OK();
}

Status ChunkManagerTargeter::refreshNow(OperationContext* txn, RefreshType refreshType) {
    if (refreshType == RefreshType_ReloadDatabase) {
        Grid::get(txn)->catalogCache()->invalidate(_nss.db().toString());
    }

    // Try not to spam the configs
    refreshBackoff();

    ScopedChunkManager::refreshAndGet(txn, _nss);

    auto scopedCMStatus = ScopedChunkManager::get(txn, _nss);
    if (!scopedCMStatus.isOK()) {
        return scopedCMStatus.getStatus();
    }

    const auto& scopedCM = scopedCMStatus.getValue();

    _manager = scopedCM.cm();
    _primary = scopedCM.primary();

    return Status::OK();
}

}  // namespace mongo
