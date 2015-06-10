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

#include <boost/thread/mutex.hpp>
#include <string>
#include <vector>

#include "mongo/s/client/shard.h"

namespace mongo {

    class BSONObjBuilder;
    class CatalogManager;
    class RemoteCommandRunner;
    class RemoteCommandTargeter;
    class RemoteCommandTargeterFactory;
    class Shard;
    class ShardType;

namespace executor {
    class TaskExecutor;
} // namespace executor

    /**
     * Maintains the set of all shards known to the MongoS instance.
     */
    class ShardRegistry {
    public:
        ShardRegistry(std::unique_ptr<RemoteCommandTargeterFactory> targeterFactory,
                      std::unique_ptr<RemoteCommandRunner> commandRunner,
                      std::unique_ptr<executor::TaskExecutor> executor,
                      CatalogManager* catalogManager);

        ~ShardRegistry();

        std::shared_ptr<RemoteCommandTargeter> getTargeterForShard(const std::string& shardId);

        RemoteCommandRunner* getCommandRunner() const { return _commandRunner.get(); }

        executor::TaskExecutor* getExecutor() const { return _executor.get(); }

        void reload();

        std::shared_ptr<Shard> findIfExists(const ShardId& id);

        /**
         * Lookup shard by replica set name. Returns nullptr if the name can't be found.
         * Note: this doesn't refresh the table if the name isn't found, so it's possible that a
         * newly added shard/Replica Set may not be found.
         */
        ShardPtr lookupRSName(const std::string& name);

        void set(const ShardId& id, const Shard& s);

        void remove(const ShardId& id);

        void getAllShardIds(std::vector<ShardId>* all) const;

        void toBSON(BSONObjBuilder* result) const;

    private:
        typedef std::map<ShardId, std::shared_ptr<Shard>> ShardMap;
        typedef std::map<ShardId, std::shared_ptr<RemoteCommandTargeter>> TargeterMap;

        /**
         * Creates a shard based on the specified information and puts it into the lookup maps.
         */
        void _addShard_inlock(const ShardType& shardType);

        std::shared_ptr<Shard> _findUsingLookUp(const ShardId& shardId);

        std::shared_ptr<RemoteCommandTargeter> _findTargeter(const std::string& shardId);

        // Factory to obtain remote command targeters for shards
        const std::unique_ptr<RemoteCommandTargeterFactory> _targeterFactory;

        // API to run remote commands to shards in a synchronous manner
        const std::unique_ptr<RemoteCommandRunner> _commandRunner;

        // Executor for scheduling work and remote commands to shards that run in an asynchronous
        // manner.
        const std::unique_ptr<executor::TaskExecutor> _executor;

        // Catalog manager from which to load the shard information. Not owned and must outlive
        // the shard registry object.
        CatalogManager* const _catalogManager;

        // Protects the maps below
        mutable boost::mutex _mutex;

        // Map of both shardName -> Shard and hostName -> Shard
        ShardMap _lookup;

        // TODO: These should eventually disappear and become parts of Shard

        // Map of shard name to targeter for this shard
        TargeterMap _targeters;

        // Map from all hosts within a replica set to the shard representing this replica set
        ShardMap _rsLookup;
    };

} // namespace mongo
