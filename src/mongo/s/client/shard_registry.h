/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "mongo/base/disallow_copying.h"
#include "mongo/db/jsobj.h"
#include "mongo/s/client/shard.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"

namespace mongo {

class BSONObjBuilder;
struct HostAndPort;
class NamespaceString;
class OperationContext;
class ShardFactory;
class Shard;
class ShardType;

class ShardRegistryData {
public:
    ShardRegistryData(OperationContext* txn, ShardFactory* shardFactory);
    ShardRegistryData() = default;
    ~ShardRegistryData() = default;


    void swap(ShardRegistryData& other);

    /**
     * Creates a shard based on the specified information and puts it into the lookup maps.
     */
    void addShard(const std::shared_ptr<Shard>&);

    /**
     * Lookup shard by replica set name. Returns nullptr if the name can't be found.
     */
    std::shared_ptr<Shard> findByRSName(const std::string& rsName) const;

    /**
     * Returns a shared pointer to the shard object with the given shard id.
     */
    std::shared_ptr<Shard> findByShardId(const ShardId&) const;

    /**
     * Finds the Shard that the mongod listening at this HostAndPort is a member of.
     */
    std::shared_ptr<Shard> findByHostAndPort(const HostAndPort&) const;

    /**
     * Returns config shard.
     */
    std::shared_ptr<Shard> getConfigShard() const;

    /**
     * Adds config shard.
     */
    void addConfigShard(std::shared_ptr<Shard>);

    void getAllShardIds(std::set<ShardId>& result) const;
    void toBSON(BSONObjBuilder* result) const;
    /**
     * If the shard with same replica set name as in the newConnString already exists then replace
     * it with the shard built for the newConnString.
     */
    void rebuildShardIfExists(const ConnectionString& newConnString, ShardFactory* factory);

    /**
     * Rebuilds config shard. The result is to recreate a ReplicaSetMonitor in the case it does
     * not exist.
     */
    void rebuildConfigShard(ShardFactory* factory);

private:
    /**
     * Reads shards docs from the catalog client and fills in maps.
     */
    void _init(OperationContext* txn, ShardFactory* factory);

    void _addShard_inlock(const std::shared_ptr<Shard>&);
    std::shared_ptr<Shard> _findByShardId_inlock(const ShardId&) const;
    void _rebuildShard_inlock(const ConnectionString& newConnString, ShardFactory* factory);

    // Protects the lookup maps below.
    mutable stdx::mutex _mutex;
    using ShardMap = std::unordered_map<ShardId, std::shared_ptr<Shard>, ShardId::Hasher>;

    // Map of both shardName -> Shard and hostName -> Shard
    ShardMap _lookup;

    // Map from replica set name to shard corresponding to this replica set
    ShardMap _rsLookup;

    std::unordered_map<HostAndPort, std::shared_ptr<Shard>> _hostLookup;

    // store configShard separately to always have a reference
    std::shared_ptr<Shard> _configShard;
};

/**
 * Maintains the set of all shards known to the instance and their connections and exposes
 * functionality to run commands against shards. All commands which this registry executes are
 * retried on NotMaster class of errors and in addition all read commands are retried on network
 * errors automatically as well.
 */
class ShardRegistry {
    MONGO_DISALLOW_COPYING(ShardRegistry);

public:
    /**
     * Instantiates a new shard registry.
     *
     * @param shardFactory Makes shards
     * @param configServerCS ConnectionString used for communicating with the config servers
     */
    ShardRegistry(std::unique_ptr<ShardFactory> shardFactory,
                  const ConnectionString& configServerCS);

    ~ShardRegistry() = default;

    /**
     *  Starts ReplicaSetMonitor by adding a config shard.
     */
    void startup();

    ConnectionString getConfigServerConnectionString() const;

    /**
     * Reloads the ShardRegistry based on the contents of the config server's config.shards
     * collection. Returns true if this call performed a reload and false if this call only waited
     * for another thread to perform the reload and did not actually reload. Because of this, it is
     * possible that calling reload once may not result in the most up to date view. If strict
     * reloading is required, the caller should call this method one more time if the first call
     * returned false.
     */
    bool reload(OperationContext* txn);

    /**
     * Throws out and reconstructs the config shard.  This has the effect that if replica set
     * monitoring of the config server replica set has stopped (because the set was down for too
     * long), this will cause the ReplicaSetMonitor to be rebuilt, which will re-trigger monitoring
     * of the config replica set to resume.
     */
    void rebuildConfigShard();

    /**
     * Takes a connection string describing either a shard or config server replica set, looks
     * up the corresponding Shard object based on the replica set name, then updates the
     * ShardRegistry's notion of what hosts make up that shard.
     */
    void updateReplSetHosts(const ConnectionString& newConnString);

    /**
     * Returns a shared pointer to the shard object with the given shard id.
     * May refresh the shard registry if there's no cached information about the shard. The shardId
     * parameter can actually be the shard name or the HostAndPort for any
     * server in the shard.
     */
    std::shared_ptr<Shard> getShard(OperationContext* txn, const ShardId& shardId);

    /**
     * Returns a shared pointer to the shard object with the given shard id. The shardId parameter
     * can actually be the shard name or the HostAndPort for any server in the shard. Will not
     * refresh the shard registry or otherwise perform any network traffic. This means that if the
     * shard was recently added it may not be found.  USE WITH CAUTION.
     */
    std::shared_ptr<Shard> getShardNoReload(const ShardId& shardId);

    /**
     * Finds the Shard that the mongod listening at this HostAndPort is a member of. Will not
     * refresh the shard registry or otherwise perform any network traffic.
     */
    std::shared_ptr<Shard> getShardForHostNoReload(const HostAndPort& shardHost);

    /**
     * Returns shared pointer to the shard object representing the config servers.
     */
    std::shared_ptr<Shard> getConfigShard() const;

    /**
     * Instantiates a new detached shard connection, which does not appear in the list of shards
     * tracked by the registry and as a result will not be returned by getAllShardIds.
     *
     * The caller owns the returned shard object and is responsible for disposing of it when done.
     *
     * @param connStr Connection string to the shard.
     */
    std::unique_ptr<Shard> createConnection(const ConnectionString& connStr) const;

    /**
     * Lookup shard by replica set name. Returns nullptr if the name can't be found.
     * Note: this doesn't refresh the table if the name isn't found, so it's possible that a
     * newly added shard/Replica Set may not be found.
     */
    std::shared_ptr<Shard> lookupRSName(const std::string& name) const;

    void getAllShardIds(std::vector<ShardId>* all) const;
    void toBSON(BSONObjBuilder* result) const;

private:
    /**
     * Factory to create shards.  Never changed after startup so safe to access outside of _mutex.
     */
    const std::unique_ptr<ShardFactory> _shardFactory;

    /**
     * Specified in the ShardRegistry c-tor. It's used only in startup() to initialize the config
     * shard
     */
    ConnectionString _initConfigServerCS;

    ShardRegistryData _data;

    // Protects the _reloadState and _initConfigServerCS during startup.
    mutable stdx::mutex _reloadMutex;
    stdx::condition_variable _inReloadCV;

    enum class ReloadState {
        Idle,       // no other thread is loading data from config server in reload().
        Reloading,  // another thread is loading data from the config server in reload().
        Failed,     // last call to reload() caused an error when contacting the config server.
    };

    ReloadState _reloadState{ReloadState::Idle};
};


}  // namespace mongo
