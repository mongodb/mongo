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

#include <vector>

#include "mongo/db/lasterror.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_database.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/grid.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/log.h"

namespace mongo {

struct CollectionInfo {
    // The config server opTime at which the chunk manager below was loaded
    const repl::OpTime configOpTime;

    // The chunk manager
    const std::shared_ptr<ChunkManager> cm;
};

DBConfig::DBConfig(const DatabaseType& dbt, repl::OpTime configOpTime)
    : _name(dbt.getName()),
      _shardingEnabled(dbt.getSharded()),
      _primaryId(dbt.getPrimary()),
      _configOpTime(std::move(configOpTime)) {}

DBConfig::~DBConfig() = default;

bool DBConfig::isSharded(const std::string& ns) {
    stdx::lock_guard<stdx::mutex> lk(_lock);

    return _collections.count(ns) > 0;
}

void DBConfig::markNSNotSharded(const std::string& ns) {
    stdx::lock_guard<stdx::mutex> lk(_lock);

    CollectionInfoMap::iterator it = _collections.find(ns);
    if (it != _collections.end()) {
        _collections.erase(it);
    }
}

std::shared_ptr<ChunkManager> DBConfig::getChunkManagerIfExists(OperationContext* txn,
                                                                const std::string& ns,
                                                                bool shouldReload,
                                                                bool forceReload) {
    // Don't report exceptions here as errors in GetLastError
    LastError::Disabled ignoreForGLE(&LastError::get(cc()));

    try {
        return getChunkManager(txn, ns, shouldReload, forceReload);
    } catch (const DBException&) {
        return nullptr;
    }
}

std::shared_ptr<ChunkManager> DBConfig::getChunkManager(OperationContext* txn,
                                                        const std::string& ns,
                                                        bool shouldReload,
                                                        bool forceReload) {
    ChunkVersion oldVersion;
    std::shared_ptr<ChunkManager> oldManager;

    {
        stdx::lock_guard<stdx::mutex> lk(_lock);

        auto it = _collections.find(ns);

        const bool earlyReload = (it == _collections.end()) && (shouldReload || forceReload);
        if (earlyReload) {
            // This is to catch cases where there this is a new sharded collection.
            // Note: read the _reloadCount inside the _lock mutex, so _loadIfNeeded will always
            // be forced to perform a reload.
            const auto currentReloadIteration = _reloadCount.load();
            _loadIfNeeded(txn, currentReloadIteration);

            it = _collections.find(ns);
        }

        uassert(ErrorCodes::NamespaceNotSharded,
                str::stream() << "Collection is not sharded: " << ns,
                it != _collections.end());

        const auto& ci = it->second;

        if (!(shouldReload || forceReload) || earlyReload) {
            return ci.cm;
        }

        if (ci.cm) {
            oldManager = ci.cm;
            oldVersion = ci.cm->getVersion();
        }
    }

    // TODO: We need to keep this first one-chunk check in until we have a more efficient way of
    // creating/reusing a chunk manager, as doing so requires copying the full set of chunks
    // currently
    std::vector<ChunkType> newestChunk;
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

                auto it = _collections.find(ns);
                uassert(15885,
                        str::stream() << "not sharded after reloading from chunks : " << ns,
                        it != _collections.end());

                const auto& ci = it->second;
                return ci.cm;
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

            auto it = _collections.find(ns);

            if (it != _collections.end()) {
                const auto& ci = it->second;

                ChunkVersion currentVersion = newestChunk[0].getVersion();

                // Only reload if the version we found is newer than our own in the same epoch
                if (currentVersion <= ci.cm->getVersion() &&
                    ci.cm->getVersion().hasEqualEpoch(currentVersion)) {
                    return ci.cm;
                }
            }
        }

        // Reload the chunk manager outside of the DBConfig's mutex so as to not block operations
        // for different collections on the same database
        tempChunkManager.reset(new ChunkManager(
            NamespaceString(oldManager->getns()),
            oldManager->getVersion().epoch(),
            oldManager->getShardKeyPattern(),
            oldManager->getDefaultCollator() ? oldManager->getDefaultCollator()->clone() : nullptr,
            oldManager->isUnique()));
        tempChunkManager->loadExistingRanges(txn, oldManager.get());

        if (!tempChunkManager->numChunks()) {
            // Maybe we're not sharded any more, so do a full reload
            const auto currentReloadIteration = _reloadCount.load();

            const bool successful = [&]() {
                stdx::lock_guard<stdx::mutex> lk(_lock);
                return _loadIfNeeded(txn, currentReloadIteration);
            }();

            // If we aren't successful loading the database entry, we don't want to keep the stale
            // object around which has invalid data.
            if (!successful) {
                Grid::get(txn)->catalogCache()->invalidate(_name);
            }

            return getChunkManager(txn, ns);
        }
    }

    stdx::lock_guard<stdx::mutex> lk(_lock);

    auto it = _collections.find(ns);
    uassert(14822,
            str::stream() << "Collection " << ns << " became unsharded in the middle.",
            it != _collections.end());

    const auto& ci = it->second;

    // Reset if our versions aren't the same
    bool shouldReset = !tempChunkManager->getVersion().equals(ci.cm->getVersion());

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

    if (shouldReset && tempChunkManager->getVersion() < ci.cm->getVersion()) {
        shouldReset = false;

        warning() << "not resetting chunk manager for collection '" << ns << "', config version is "
                  << tempChunkManager->getVersion() << " and "
                  << "old version is " << ci.cm->getVersion();
    }

    // end legacy behavior

    if (shouldReset) {
        const auto cmOpTime = tempChunkManager->getConfigOpTime();

        // The existing ChunkManager could have been updated since we last checked, so replace the
        // existing chunk manager only if it is strictly newer.
        if (cmOpTime > ci.cm->getConfigOpTime()) {
            _collections.erase(ns);
            auto emplacedEntryIt =
                _collections.emplace(ns, CollectionInfo{cmOpTime, std::move(tempChunkManager)})
                    .first;
            return emplacedEntryIt->second.cm;
        }
    }

    return ci.cm;
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
    std::vector<CollectionType> collections;
    repl::OpTime configOpTimeWhenLoadingColl;
    uassertStatusOK(
        catalogClient->getCollections(txn, &_name, &collections, &configOpTimeWhenLoadingColl));

    invariant(configOpTimeWhenLoadingColl >= _configOpTime);

    for (const auto& coll : collections) {
        auto collIter = _collections.find(coll.getNs().ns());
        if (collIter != _collections.end()) {
            invariant(configOpTimeWhenLoadingColl >= collIter->second.configOpTime);
        }

        _collections.erase(coll.getNs().ns());

        if (!coll.getDropped()) {
            std::unique_ptr<CollatorInterface> defaultCollator;
            if (!coll.getDefaultCollation().isEmpty()) {
                auto statusWithCollator = CollatorFactoryInterface::get(txn->getServiceContext())
                                              ->makeFromBSON(coll.getDefaultCollation());

                // The collation was validated upon collection creation.
                invariantOK(statusWithCollator.getStatus());

                defaultCollator = std::move(statusWithCollator.getValue());
            }

            std::unique_ptr<ChunkManager> manager(
                stdx::make_unique<ChunkManager>(coll.getNs(),
                                                coll.getEpoch(),
                                                ShardKeyPattern(coll.getKeyPattern()),
                                                std::move(defaultCollator),
                                                coll.getUnique()));

            // Do the blocking collection load
            manager->loadExistingRanges(txn, nullptr);

            // Collections with no chunks are unsharded, no matter what the collections entry says
            if (manager->numChunks()) {
                _collections.emplace(
                    coll.getNs().ns(),
                    CollectionInfo{configOpTimeWhenLoadingColl, std::move(manager)});
            }
        }
    }

    _reloadCount.fetchAndAdd(1);

    return true;
}

ShardId DBConfig::getPrimaryId() {
    stdx::lock_guard<stdx::mutex> lk(_lock);
    return _primaryId;
}

}  // namespace mongo
