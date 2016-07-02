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

#include "mongo/client/connpool.h"
#include "mongo/db/client.h"
#include "mongo/db/lasterror.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/write_concern.h"
#include "mongo/s/balancer/balancer_configuration.h"
#include "mongo/s/catalog/catalog_cache.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_database.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/cluster_write.h"
#include "mongo/s/grid.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/log.h"

namespace mongo {

using std::set;
using std::string;
using std::unique_ptr;
using std::vector;

CollectionInfo::CollectionInfo(OperationContext* txn,
                               const CollectionType& coll,
                               repl::OpTime opTime)
    : _configOpTime(std::move(opTime)) {
    _dropped = coll.getDropped();

    // Do this *first* so we're invisible to everyone else
    std::unique_ptr<ChunkManager> manager(stdx::make_unique<ChunkManager>(coll));
    manager->loadExistingRanges(txn, nullptr);

    // Collections with no chunks are unsharded, no matter what the collections entry says. This
    // helps prevent errors when dropping in a different process.
    if (manager->numChunks() != 0) {
        useChunkManager(std::move(manager));
    } else {
        warning() << "no chunks found for collection " << manager->getns()
                  << ", assuming unsharded";
        unshard();
    }

    _dirty = false;
}

CollectionInfo::~CollectionInfo() = default;

void CollectionInfo::resetCM(ChunkManager* cm) {
    invariant(cm);
    invariant(_cm);

    _cm.reset(cm);
}

void CollectionInfo::unshard() {
    _cm.reset();
    _dropped = true;
    _dirty = true;
    _key = BSONObj();
}

void CollectionInfo::useChunkManager(std::shared_ptr<ChunkManager> manager) {
    _cm = manager;
    _key = manager->getShardKeyPattern().toBSON().getOwned();
    _unique = manager->isUnique();
    _dirty = true;
    _dropped = false;
}

void CollectionInfo::save(OperationContext* txn, const string& ns) {
    CollectionType coll;
    coll.setNs(NamespaceString{ns});

    if (_cm) {
        invariant(!_dropped);
        coll.setEpoch(_cm->getVersion().epoch());
        // TODO(schwerin): The following isn't really a date, but is stored as one in-memory and
        // in config.collections, as a historical oddity.
        coll.setUpdatedAt(Date_t::fromMillisSinceEpoch(_cm->getVersion().toLong()));
        coll.setKeyPattern(_cm->getShardKeyPattern().toBSON());
        coll.setUnique(_cm->isUnique());
    } else {
        invariant(_dropped);
        coll.setDropped(true);
        coll.setEpoch(ChunkVersion::DROPPED().epoch());
        coll.setUpdatedAt(Date_t::now());
    }

    uassertStatusOK(grid.catalogClient(txn)->updateCollection(txn, ns, coll));
    _dirty = false;
}

DBConfig::DBConfig(std::string name, const DatabaseType& dbt, repl::OpTime configOpTime)
    : _name(name), _configOpTime(std::move(configOpTime)) {
    invariant(_name == dbt.getName());
    _primaryId = dbt.getPrimary();
    _shardingEnabled = dbt.getSharded();
}

DBConfig::~DBConfig() = default;

bool DBConfig::isSharded(const string& ns) {
    stdx::lock_guard<stdx::mutex> lk(_lock);

    if (!_shardingEnabled) {
        return false;
    }

    CollectionInfoMap::iterator i = _collections.find(ns);
    if (i == _collections.end()) {
        return false;
    }

    return i->second.isSharded();
}

const ShardId& DBConfig::getShardId(OperationContext* txn, const string& ns) {
    uassert(28679, "ns can't be sharded", !isSharded(ns));

    uassert(10178, "no primary!", grid.shardRegistry()->getShard(txn, _primaryId));
    return _primaryId;
}

void DBConfig::invalidateNs(const std::string& ns) {
    stdx::lock_guard<stdx::mutex> lk(_lock);

    CollectionInfoMap::iterator it = _collections.find(ns);
    if (it != _collections.end()) {
        _collections.erase(it);
    }
}

void DBConfig::enableSharding(OperationContext* txn) {
    invariant(_name != "config");

    stdx::lock_guard<stdx::mutex> lk(_lock);

    if (_shardingEnabled) {
        return;
    }

    _shardingEnabled = true;
    _save(txn);
}

bool DBConfig::removeSharding(OperationContext* txn, const string& ns) {
    if (!_shardingEnabled) {
        warning() << "could not remove sharding for collection " << ns
                  << ", sharding not enabled for db";
        return false;
    }

    stdx::lock_guard<stdx::mutex> lk(_lock);

    CollectionInfoMap::iterator i = _collections.find(ns);

    if (i == _collections.end())
        return false;

    CollectionInfo& ci = _collections[ns];
    if (!ci.isSharded()) {
        warning() << "could not remove sharding for collection " << ns
                  << ", no sharding information found";
        return false;
    }

    ci.unshard();
    _save(txn, false, true);
    return true;
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

    {
        stdx::lock_guard<stdx::mutex> lk(_lock);

        CollectionInfoMap::iterator i = _collections.find(ns);

        // No namespace
        if (i == _collections.end()) {
            // If we don't know about this namespace, it's unsharded by default
            primary = grid.shardRegistry()->getShard(txn, _primaryId);
        } else {
            CollectionInfo& cInfo = i->second;

            // TODO: we need to be careful about handling shardingEnabled, b/c in some places we
            // seem to use and some we don't.  If we use this function in combination with just
            // getChunkManager() on a slightly borked config db, we'll get lots of staleconfig
            // retries
            if (_shardingEnabled && cInfo.isSharded()) {
                manager = cInfo.getCM();
            } else {
                primary = grid.shardRegistry()->getShard(txn, _primaryId);
            }
        }
    }

    verify(manager || primary);
    verify(!manager || !primary);
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
    BSONObj key;
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

        CollectionInfo& ci = _collections[ns];
        uassert(10181, str::stream() << "not sharded:" << ns, ci.isSharded());

        invariant(!ci.key().isEmpty());

        if (!(shouldReload || forceReload) || earlyReload) {
            return ci.getCM();
        }

        key = ci.key().copy();

        if (ci.getCM()) {
            oldManager = ci.getCM();
            oldVersion = ci.getCM()->getVersion();
        }
    }

    invariant(!key.isEmpty());

    // TODO: We need to keep this first one-chunk check in until we have a more efficient way of
    // creating/reusing a chunk manager, as doing so requires copying the full set of chunks
    // currently
    vector<ChunkType> newestChunk;
    if (oldVersion.isSet() && !forceReload) {
        uassertStatusOK(
            grid.catalogClient(txn)->getChunks(txn,
                                               BSON(ChunkType::ns(ns)),
                                               BSON(ChunkType::DEPRECATED_lastmod() << -1),
                                               1,
                                               &newestChunk,
                                               nullptr));

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

    // we are not locked now, and want to load a new ChunkManager

    unique_ptr<ChunkManager> tempChunkManager;

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

        tempChunkManager.reset(new ChunkManager(
            oldManager->getns(), oldManager->getShardKeyPattern(), oldManager->isUnique()));
        tempChunkManager->loadExistingRanges(txn, oldManager.get());

        if (tempChunkManager->numChunks() == 0) {
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

void DBConfig::setPrimary(OperationContext* txn, const ShardId& newPrimaryId) {
    stdx::lock_guard<stdx::mutex> lk(_lock);
    _primaryId = newPrimaryId;
    _save(txn);
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

    auto status = grid.catalogClient(txn)->getDatabase(txn, _name);
    if (status == ErrorCodes::NamespaceNotFound) {
        return false;
    }

    // All other errors are connectivity, etc so throw an exception.
    uassertStatusOK(status.getStatus());

    const auto& dbOpTimePair = status.getValue();
    const auto& dbt = dbOpTimePair.value;
    invariant(_name == dbt.getName());
    _primaryId = dbt.getPrimary();
    _shardingEnabled = dbt.getSharded();

    invariant(dbOpTimePair.opTime >= _configOpTime);
    _configOpTime = dbOpTimePair.opTime;

    // Load all collections
    vector<CollectionType> collections;
    repl::OpTime configOpTimeWhenLoadingColl;
    uassertStatusOK(grid.catalogClient(txn)->getCollections(
        txn, &_name, &collections, &configOpTimeWhenLoadingColl));

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

void DBConfig::_save(OperationContext* txn, bool db, bool coll) {
    if (db) {
        DatabaseType dbt;
        dbt.setName(_name);
        dbt.setPrimary(_primaryId);
        dbt.setSharded(_shardingEnabled);

        uassertStatusOK(grid.catalogClient(txn)->updateDatabase(txn, _name, dbt));
    }

    if (coll) {
        for (CollectionInfoMap::iterator i = _collections.begin(); i != _collections.end(); ++i) {
            if (!i->second.isDirty()) {
                continue;
            }

            i->second.save(txn, i->first);
        }
    }
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
        grid.catalogCache()->invalidate(_name);
    }

    return successful;
}

bool DBConfig::dropDatabase(OperationContext* txn, string& errmsg) {
    /**
     * 1) update config server
     * 2) drop and reset sharded collections
     * 3) drop and reset primary
     * 4) drop everywhere to clean up loose ends
     */

    log() << "DBConfig::dropDatabase: " << _name;
    grid.catalogClient(txn)->logChange(txn, "dropDatabase.start", _name, BSONObj());

    // 1
    grid.catalogCache()->invalidate(_name);

    Status result = grid.catalogClient(txn)->removeConfigDocuments(
        txn,
        DatabaseType::ConfigNS,
        BSON(DatabaseType::name(_name)),
        ShardingCatalogClient::kMajorityWriteConcern);
    if (!result.isOK()) {
        errmsg = result.reason();
        log() << "could not drop '" << _name << "': " << errmsg;
        return false;
    }

    LOG(1) << "\t removed entry from config server for: " << _name;

    set<ShardId> shardIds;

    // 2
    while (true) {
        int num = 0;
        if (!_dropShardedCollections(txn, num, shardIds, errmsg)) {
            return 0;
        }

        log() << "   DBConfig::dropDatabase: " << _name << " dropped sharded collections: " << num;

        if (num == 0) {
            break;
        }
    }

    // 3
    {
        const auto shard = grid.shardRegistry()->getShard(txn, _primaryId);
        ScopedDbConnection conn(shard->getConnString(), 30.0);
        BSONObj res;
        if (!conn->dropDatabase(_name, txn->getWriteConcern(), &res)) {
            errmsg = res.toString() + " at " + _primaryId.toString();
            return 0;
        }
        conn.done();
        if (auto wcErrorElem = res["writeConcernError"]) {
            auto wcError = wcErrorElem.Obj();
            if (auto errMsgElem = wcError["errmsg"]) {
                errmsg = errMsgElem.str() + " at " + _primaryId.toString();
                return false;
            }
        }
    }

    // 4
    for (const ShardId& shardId : shardIds) {
        const auto shard = grid.shardRegistry()->getShard(txn, shardId);
        if (!shard) {
            continue;
        }

        ScopedDbConnection conn(shard->getConnString(), 30.0);
        BSONObj res;
        if (!conn->dropDatabase(_name, txn->getWriteConcern(), &res)) {
            errmsg = res.toString() + " at " + shardId.toString();
            return 0;
        }
        conn.done();
        if (auto wcErrorElem = res["writeConcernError"]) {
            auto wcError = wcErrorElem.Obj();
            if (auto errMsgElem = wcError["errmsg"]) {
                errmsg = errMsgElem.str() + " at " + shardId.toString();
                return false;
            }
        }
    }

    LOG(1) << "\t dropped primary db for: " << _name;

    grid.catalogClient(txn)->logChange(txn, "dropDatabase", _name, BSONObj());

    return true;
}

bool DBConfig::_dropShardedCollections(OperationContext* txn,
                                       int& num,
                                       set<ShardId>& shardIds,
                                       string& errmsg) {
    num = 0;
    set<string> seen;
    while (true) {
        CollectionInfoMap::iterator i = _collections.begin();
        for (; i != _collections.end(); ++i) {
            if (i->second.isSharded()) {
                break;
            }
        }

        if (i == _collections.end()) {
            break;
        }

        if (seen.count(i->first)) {
            errmsg = "seen a collection twice!";
            return false;
        }

        seen.insert(i->first);
        LOG(1) << "\t dropping sharded collection: " << i->first;

        i->second.getCM()->getAllShardIds(&shardIds);

        uassertStatusOK(grid.catalogClient(txn)->dropCollection(txn, NamespaceString(i->first)));

        // We should warn, but it's not a fatal error if someone else reloaded the db/coll as
        // unsharded in the meantime
        if (!removeSharding(txn, i->first)) {
            warning() << "collection " << i->first
                      << " was reloaded as unsharded before drop completed"
                      << " during drop of all collections";
        }

        num++;
        uassert(10184, "_dropShardedCollections too many collections - bailing", num < 100000);
        LOG(2) << "\t\t dropped " << num << " so far";
    }

    return true;
}

void DBConfig::getAllShardIds(set<ShardId>* shardIds) {
    dassert(shardIds);

    stdx::lock_guard<stdx::mutex> lk(_lock);
    shardIds->insert(getPrimaryId());
    for (CollectionInfoMap::const_iterator it(_collections.begin()), end(_collections.end());
         it != end;
         ++it) {
        if (it->second.isSharded()) {
            it->second.getCM()->getAllShardIds(shardIds);
        }  // TODO: handle collections on non-primary shard
    }
}

void DBConfig::getAllShardedCollections(set<string>& namespaces) {
    stdx::lock_guard<stdx::mutex> lk(_lock);

    for (CollectionInfoMap::const_iterator i = _collections.begin(); i != _collections.end(); i++) {
        log() << "Coll : " << i->first << " sharded? " << i->second.isSharded();
        if (i->second.isSharded())
            namespaces.insert(i->first);
    }
}

/* --- ConfigServer ---- */

void ConfigServer::replicaSetChangeShardRegistryUpdateHook(const string& setName,
                                                           const string& newConnectionString) {
    // Inform the ShardRegsitry of the new connection string for the shard.
    auto connString = fassertStatusOK(28805, ConnectionString::parse(newConnectionString));
    invariant(setName == connString.getSetName());
    grid.shardRegistry()->updateReplSetHosts(connString);
}

void ConfigServer::replicaSetChangeConfigServerUpdateHook(const string& setName,
                                                          const string& newConnectionString) {
    // This is run in it's own thread. Exceptions escaping would result in a call to terminate.
    Client::initThread("replSetChange");
    auto txn = cc().makeOperationContext();

    try {
        std::shared_ptr<Shard> s = grid.shardRegistry()->lookupRSName(setName);
        if (!s) {
            LOG(1) << "shard not found for set: " << newConnectionString
                   << " when attempting to inform config servers of updated set membership";
            return;
        }

        if (s->isConfig()) {
            // No need to tell the config servers their own connection string.
            return;
        }

        auto status = grid.catalogClient(txn.get())->updateConfigDocument(
            txn.get(),
            ShardType::ConfigNS,
            BSON(ShardType::name(s->getId().toString())),
            BSON("$set" << BSON(ShardType::host(newConnectionString))),
            false,
            ShardingCatalogClient::kMajorityWriteConcern);
        if (!status.isOK()) {
            error() << "RSChangeWatcher: could not update config db for set: " << setName
                    << " to: " << newConnectionString << causedBy(status.getStatus());
        }
    } catch (const std::exception& e) {
        warning() << "caught exception while updating config servers: " << e.what();
    } catch (...) {
        warning() << "caught unknown exception while updating config servers";
    }
}

}  // namespace mongo
