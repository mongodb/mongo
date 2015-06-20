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

#pragma once

#include <set>

#include "mongo/db/jsobj.h"
#include "mongo/s/client/shard.h"
#include "mongo/util/concurrency/mutex.h"

namespace mongo {

class ChunkManager;
class CollectionType;
class DatabaseType;
class DBConfig;

typedef std::shared_ptr<DBConfig> DBConfigPtr;

struct CollectionInfo {
    CollectionInfo() {
        _dirty = false;
        _dropped = false;
    }

    CollectionInfo(const CollectionType& in);
    ~CollectionInfo();

    bool isSharded() const {
        return _cm.get();
    }

    std::shared_ptr<ChunkManager> getCM() const {
        return _cm;
    }

    void resetCM(ChunkManager* cm);

    void shard(ChunkManager* cm);
    void unshard();

    bool isDirty() const {
        return _dirty;
    }
    bool wasDropped() const {
        return _dropped;
    }

    void save(const std::string& ns);

    void useChunkManager(std::shared_ptr<ChunkManager> manager);

    bool unique() const {
        return _unique;
    }
    BSONObj key() const {
        return _key;
    }

private:
    BSONObj _key;
    bool _unique;
    std::shared_ptr<ChunkManager> _cm;
    bool _dirty;
    bool _dropped;
};

/**
 * top level configuration for a database
 */
class DBConfig {
public:
    DBConfig(std::string name, const DatabaseType& dbt);

    /**
     * The name of the database which this entry caches.
     */
    const std::string& name() const {
        return _name;
    }

    /**
     * Whether sharding is enabled for this database.
     */
    bool isShardingEnabled() const {
        return _shardingEnabled;
    }

    const ShardId& getPrimaryId() const {
        return _primaryId;
    }

    void enableSharding(bool save = true);

    /**
       @return true if there was sharding info to remove
     */
    bool removeSharding(const std::string& ns);

    /**
     * @return whether or not the 'ns' collection is partitioned
     */
    bool isSharded(const std::string& ns);

    // Atomically returns *either* the chunk manager *or* the primary shard for the collection,
    // neither if the collection doesn't exist.
    void getChunkManagerOrPrimary(const std::string& ns,
                                  std::shared_ptr<ChunkManager>& manager,
                                  std::shared_ptr<Shard>& primary);

    std::shared_ptr<ChunkManager> getChunkManager(const std::string& ns,
                                                  bool reload = false,
                                                  bool forceReload = false);
    std::shared_ptr<ChunkManager> getChunkManagerIfExists(const std::string& ns,
                                                          bool reload = false,
                                                          bool forceReload = false);

    /**
     * Returns shard id for primary shard for the database for which this DBConfig represents.
     */
    const ShardId& getShardId(const std::string& ns);

    void setPrimary(const std::string& s);

    bool load();
    bool reload();

    bool dropDatabase(std::string& errmsg);

    void getAllShardIds(std::set<ShardId>* shardIds);
    void getAllShardedCollections(std::set<std::string>& namespaces);

protected:
    typedef std::map<std::string, CollectionInfo> CollectionInfoMap;


    /**
        lockless
    */
    bool _isSharded(const std::string& ns);

    bool _dropShardedCollections(int& num, std::set<ShardId>& shardIds, std::string& errmsg);

    bool _load();
    bool _reload();
    void _save(bool db = true, bool coll = true);


    // Name of the database which this entry caches
    const std::string _name;

    // Primary shard id
    ShardId _primaryId;

    // Whether sharding has been enabled for this database
    bool _shardingEnabled;

    // Set of collections and lock to protect access
    stdx::mutex _lock;
    CollectionInfoMap _collections;

    // Ensures that only one thread at a time loads collection configuration data from
    // the config server
    stdx::mutex _hitConfigServerLock;
};


class ConfigServer {
public:
    static void reloadSettings();

    static void replicaSetChange(const std::string& setName,
                                 const std::string& newConnectionString);
};

}  // namespace mongo
