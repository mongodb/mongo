/**
 *    Copyright (C) 2008-2015 MongoDB Inc.
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

#include "mongo/s/config.h"

#include "mongo/db/lasterror.h"
#include "mongo/s/catalog/catalog_cache.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_database.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/log.h"

namespace mongo {

using std::set;
using std::string;
using std::vector;

CollectionInfo::CollectionInfo(OperationContext* txn,
                               const CollectionType& coll,
                               repl::OpTime opTime)
    : _configOpTime(std::move(opTime)) {
    // Do this *first* so we're invisible to everyone else
    std::unique_ptr<ChunkManager> manager(stdx::make_unique<ChunkManager>(txn, coll));
    manager->loadExistingRanges(txn, nullptr);

    // Collections with no chunks are unsharded, no matter what the collections entry says. This
    // helps prevent errors when dropping in a different process.
    if (manager->numChunks()) {
        _cm = std::move(manager);
    }
}

CollectionInfo::~CollectionInfo() = default;

void CollectionInfo::resetCM(ChunkManager* cm) {
    invariant(cm);
    invariant(_cm);

    _cm.reset(cm);
}

DBConfig::DBConfig(const DatabaseType& dbt, repl::OpTime configOpTime)
    : _name(dbt.getName()),
      _shardingEnabled(dbt.getSharded()),
      _primaryId(dbt.getPrimary()),
      _configOpTime(std::move(configOpTime)) {}

DBConfig::~DBConfig() = default;

bool DBConfig::isSharded(const string& ns) {
    stdx::lock_guard<stdx::mutex> lk(_lock);

    CollectionInfoMap::iterator i = _collections.find(ns);
    if (i == _collections.end()) {
        return false;
    }

    return i->second.isSharded();
}

void DBConfig::markNSNotSharded(const std::string& ns) {
    stdx::lock_guard<stdx::mutex> lk(_lock);

    CollectionInfoMap::iterator it = _collections.find(ns);
    if (it != _collections.end()) {
        _collections.erase(it);
    }
}

// Handles weird logic related to getting *either* a chunk manager *or* the collection primary
// shard
void DBConfig::getChunkManagerOrPrimary(OperationContext* txn,
                                        const string& ns,
                                        std::shared_ptr<ChunkManager>& manager,
                                        std::shared_ptr<Shard>& primary) {
    // The logic here is basically that at any time, our collection can become sharded or
    // unsharded
    // via a command.  If we're not sharded, we want to send data to the primary, if sharded, we
    // want to send data to the correct chunks, and we can't check both w/o the lock.

    manager.reset();
    primary.reset();

    const auto shardRegistry = Grid::get(txn)->shardRegistry();

    {
        stdx::lock_guard<stdx::mutex> lk(_lock);

        CollectionInfoMap::iterator i = _collections.find(ns);

        // No namespace
        if (i == _collections.end()) {
            // If we don't know about this namespace, it's unsharded by default
            auto primaryStatus = shardRegistry->getShard(txn, _primaryId);
            if (primaryStatus.isOK()) {
                primary = primaryStatus.getValue();
            }
        } else {
            CollectionInfo& cInfo = i->second;

            if (cInfo.isSharded()) {
                manager = cInfo.getCM();
            } else {
                auto primaryStatus = shardRegistry->getShard(txn, _primaryId);
                if (primaryStatus.isOK()) {
                    primary = primaryStatus.getValue();
                }
            }
        }
    }

    invariant(manager || primary);
    invariant(!manager || !primary);
}


std::shared_ptr<ChunkManager> DBConfig::getChunkManagerIfExists(OperationContext* txn,
                                                                const string& ns,
                                                                bool shouldReload,
                                                                bool forceReload) {
    // Don't report exceptions here as errors in GetLastError
    LastError::Disabled ignoreForGLE(&LastError::get(cc()));

    try {
        return getChunkManager(txn, ns, shouldReload, forceReload);
    } catch (AssertionException& e) {
        warning() << "chunk manager not found for " << ns << causedBy(e);
        return nullptr;
    }
}

std::shared_ptr<ChunkManager> DBConfig::getChunkManager(OperationContext* txn,
                                                        const string& ns,
                                                        bool shouldReload,
                                                        bool forceReload) {
    ChunkVersion oldVersion;
    std::shared_ptr<ChunkManager> oldManager;

    {
        stdx::lock_guard<stdx::mutex> lk(_lock);

        bool earlyReload = !_collections[ns].isSharded() && (shouldReload || forceReload);
        if (earlyReload) {
            // This is to catch cases where there this is a new sharded collection.
            // Note: read the _reloadCount inside the _lock mutex, so _loadIfNeeded will always
            // be forced to perform a reload.
            const auto currentReloadIteration = _reloadCount.load();
            _loadIfNeeded(txn, currentReloadIteration);
        }

        const CollectionInfo& ci = _collections[ns];
        uassert(ErrorCodes::NamespaceNotSharded,
                str::stream() << "Collection is not sharded: " << ns,
                ci.isSharded());

        if (!(shouldReload || forceReload) || earlyReload) {
            return ci.getCM();
        }

        if (ci.getCM()) {
            oldManager = ci.getCM();
            oldVersion = ci.getCM()->getVersion();
        }
    }

    // TODO: We need to keep this first one-chunk check in until we have a more efficient way of
    // creating/reusing a chunk manager, as doing so requires copying the full set of chunks
    // currently
    vector<ChunkType> newestChunk;
    if (oldVersion.isSet() && !forceReload) {
        uassertStatusOK(Grid::get(txn)->catalogClient(txn)->getChunks(
            txn,
            BSON(ChunkType::ns(ns)),
            BSON(ChunkType::DEPRECATED_lastmod() << -1),
            1,
            &newestChunk,
            nullptr,
            repl::ReadConcernLevel::kMajorityReadConcern));

        if (!newestChunk.empty()) {
            invariant(newestChunk.size() == 1);
            ChunkVersion v = newestChunk[0].getVersion();
            if (v.equals(oldVersion)) {
                stdx::lock_guard<stdx::mutex> lk(_lock);
                const CollectionInfo& ci = _collections[ns];
                uassert(15885,
                        str::stream() << "not sharded after reloading from chunks : " << ns,
                        ci.isSharded());
                return ci.getCM();
            }
        }

    } else if (!oldVersion.isSet()) {
        warning() << "version 0 found when " << (forceReload ? "reloading" : "checking")
                  << " chunk manager; collection '" << ns << "' initially detected as sharded";
    }

    std::unique_ptr<ChunkManager> tempChunkManager;

    {
        stdx::lock_guard<stdx::mutex> lll(_hitConfigServerLock);

        if (!newestChunk.empty() && !forceReload) {
            // If we have a target we're going for see if we've hit already
            stdx::lock_guard<stdx::mutex> lk(_lock);

            CollectionInfo& ci = _collections[ns];

            if (ci.isSharded() && ci.getCM()) {
                ChunkVersion currentVersion = newestChunk[0].getVersion();

                // Only reload if the version we found is newer than our own in the same epoch
                if (currentVersion <= ci.getCM()->getVersion() &&
                    ci.getCM()->getVersion().hasEqualEpoch(currentVersion)) {
                    return ci.getCM();
                }
            }
        }

        // Reload the chunk manager outside of the DBConfig's mutex so as to not block operations
        // for different collections on the same database
        tempChunkManager.reset(new ChunkManager(
            oldManager->getns(),
            oldManager->getShardKeyPattern(),
            oldManager->getDefaultCollator() ? oldManager->getDefaultCollator()->clone() : nullptr,
            oldManager->isUnique()));
        tempChunkManager->loadExistingRanges(txn, oldManager.get());

        if (!tempChunkManager->numChunks()) {
            // Maybe we're not sharded any more, so do a full reload
            reload(txn);

            return getChunkManager(txn, ns, false);
        }
    }

    stdx::lock_guard<stdx::mutex> lk(_lock);

    CollectionInfo& ci = _collections[ns];
    uassert(14822, (string) "state changed in the middle: " + ns, ci.isSharded());

    // Reset if our versions aren't the same
    bool shouldReset = !tempChunkManager->getVersion().equals(ci.getCM()->getVersion());

    // Also reset if we're forced to do so
    if (!shouldReset && forceReload) {
        shouldReset = true;
        warning() << "chunk manager reload forced for collection '" << ns << "', config version is "
                  << tempChunkManager->getVersion();
    }

    //
    // LEGACY BEHAVIOR
    //
    // It's possible to get into a state when dropping collections when our new version is
    // less than our prev version. Behave identically to legacy mongos, for now, and warn to
    // draw attention to the problem.
    //
    // TODO: Assert in next version, to allow smooth upgrades
    //

    if (shouldReset && tempChunkManager->getVersion() < ci.getCM()->getVersion()) {
        shouldReset = false;

        warning() << "not resetting chunk manager for collection '" << ns << "', config version is "
                  << tempChunkManager->getVersion() << " and "
                  << "old version is " << ci.getCM()->getVersion();
    }

    // end legacy behavior

    if (shouldReset) {
        const auto cmOpTime = tempChunkManager->getConfigOpTime();

        // The existing ChunkManager could have been updated since we last checked, so
        // replace the existing chunk manager only if it is strictly newer.
        // The condition should be (>) than instead of (>=), but use (>=) since legacy non-repl
        // config servers will always have an opTime of zero.
        if (cmOpTime >= ci.getCM()->getConfigOpTime()) {
            ci.resetCM(tempChunkManager.release());
        }
    }

    uassert(
        15883, str::stream() << "not sharded after chunk manager reset : " << ns, ci.isSharded());

    return ci.getCM();
}

bool DBConfig::load(OperationContext* txn) {
    const auto currentReloadIteration = _reloadCount.load();
    stdx::lock_guard<stdx::mutex> lk(_lock);
    return _loadIfNeeded(txn, currentReloadIteration);
}

bool DBConfig::_loadIfNeeded(OperationContext* txn, Counter reloadIteration) {
    if (reloadIteration != _reloadCount.load()) {
        return true;
    }

    const auto catalogClient = Grid::get(txn)->catalogClient(txn);

    auto status = catalogClient->getDatabase(txn, _name);
    if (status == ErrorCodes::NamespaceNotFound) {
        return false;
    }

    // All other errors are connectivity, etc so throw an exception.
    uassertStatusOK(status.getStatus());

    const auto& dbOpTimePair = status.getValue();
    const auto& dbt = dbOpTimePair.value;
    invariant(_name == dbt.getName());
    _primaryId = dbt.getPrimary();

    invariant(dbOpTimePair.opTime >= _configOpTime);
    _configOpTime = dbOpTimePair.opTime;

    // Load all collections
    vector<CollectionType> collections;
    repl::OpTime configOpTimeWhenLoadingColl;
    uassertStatusOK(
        catalogClient->getCollections(txn, &_name, &collections, &configOpTimeWhenLoadingColl));

    int numCollsErased = 0;
    int numCollsSharded = 0;

    invariant(configOpTimeWhenLoadingColl >= _configOpTime);

    for (const auto& coll : collections) {
        auto collIter = _collections.find(coll.getNs().ns());
        if (collIter != _collections.end()) {
            invariant(configOpTimeWhenLoadingColl >= collIter->second.getConfigOpTime());
        }

        if (coll.getDropped()) {
            _collections.erase(coll.getNs().ns());
            numCollsErased++;
        } else {
            _collections[coll.getNs().ns()] =
                CollectionInfo(txn, coll, configOpTimeWhenLoadingColl);
            numCollsSharded++;
        }
    }

    LOG(2) << "found " << numCollsSharded << " collections left and " << numCollsErased
           << " collections dropped for database " << _name;

    _reloadCount.fetchAndAdd(1);

    return true;
}

bool DBConfig::reload(OperationContext* txn) {
    bool successful = false;
    const auto currentReloadIteration = _reloadCount.load();

    {
        stdx::lock_guard<stdx::mutex> lk(_lock);
        successful = _loadIfNeeded(txn, currentReloadIteration);
    }

    // If we aren't successful loading the database entry, we don't want to keep the stale
    // object around which has invalid data.
    if (!successful) {
        Grid::get(txn)->catalogCache()->invalidate(_name);
    }

    return successful;
}

void DBConfig::getAllShardIds(set<ShardId>* shardIds) {
    dassert(shardIds);

    stdx::lock_guard<stdx::mutex> lk(_lock);
    shardIds->insert(_primaryId);
    for (CollectionInfoMap::const_iterator it(_collections.begin()), end(_collections.end());
         it != end;
         ++it) {
        if (it->second.isSharded()) {
            it->second.getCM()->getAllShardIds(shardIds);
        }  // TODO: handle collections on non-primary shard
    }
}

ShardId DBConfig::getPrimaryId() {
    stdx::lock_guard<stdx::mutex> lk(_lock);
    return _primaryId;
}

}  // namespace mongo
