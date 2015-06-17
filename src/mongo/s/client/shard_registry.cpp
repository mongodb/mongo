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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/s/client/shard_registry.h"

#include "mongo/client/connection_string.h"
#include "mongo/client/remote_command_runner_impl.h"
#include "mongo/client/remote_command_targeter.h"
#include "mongo/client/remote_command_targeter_factory.h"
#include "mongo/executor/task_executor.h"
#include "mongo/s/catalog/catalog_manager.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/client/shard.h"
#include "mongo/stdx/memory.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    using std::shared_ptr;
    using std::string;
    using std::vector;

    ShardRegistry::ShardRegistry(std::unique_ptr<RemoteCommandTargeterFactory> targeterFactory,
                                 std::unique_ptr<RemoteCommandRunner> commandRunner,
                                 std::unique_ptr<executor::TaskExecutor> executor,
                                 CatalogManager* catalogManager)
        :  _targeterFactory(std::move(targeterFactory)),
           _commandRunner(std::move(commandRunner)),
           _executor(std::move(executor)),
           _catalogManager(catalogManager) {

    }

    ShardRegistry::~ShardRegistry() = default;

    shared_ptr<RemoteCommandTargeter> ShardRegistry::getTargeterForShard(const string& shardId) {
        auto targeter = _findTargeter(shardId);
        if (targeter) {
            return targeter;
        }

        // If we can't find the shard, we might just need to reload the cache
        reload();

        return _findTargeter(shardId);
    }

    void ShardRegistry::reload() {
        vector<ShardType> shards;
        Status status = _catalogManager->getAllShards(&shards);
        massert(13632, "couldn't get updated shard list from config server", status.isOK());

        int numShards = shards.size();

        LOG(1) << "found " << numShards << " shards listed on config server(s)";

        std::lock_guard<std::mutex> lk(_mutex);

        _lookup.clear();
        _targeters.clear();
        _rsLookup.clear();

        ShardType configServerShard;
        configServerShard.setName("config");
        configServerShard.setHost(_catalogManager->connectionString().toString());

        _addShard_inlock(configServerShard);

        for (const ShardType& shardType : shards) {
            uassertStatusOK(shardType.validate());

            // Skip the config host even if there is one left over from legacy installations. The
            // config host is installed manually from the catalog manager data.
            if (shardType.getName() == "config") {
                continue;
            }

            _addShard_inlock(shardType);
        }
    }

    shared_ptr<Shard> ShardRegistry::findIfExists(const ShardId& id) {
        shared_ptr<Shard> shard = _findUsingLookUp(id);
        if (shard) {
            return shard;
        }

        // If we can't find the shard, we might just need to reload the cache
        reload();

        return _findUsingLookUp(id);
    }

    shared_ptr<Shard> ShardRegistry::lookupRSName(const string& name) const {
        std::lock_guard<std::mutex> lk(_mutex);
        ShardMap::const_iterator i = _rsLookup.find(name);

        return (i == _rsLookup.end()) ? nullptr : i->second;
    }

    void ShardRegistry::set(const ShardId& id, const Shard& s) {
        shared_ptr<Shard> ss(std::make_shared<Shard>(s.getId(), s.getConnString()));

        std::lock_guard<std::mutex> lk(_mutex);
        _lookup[id] = ss;
    }

    void ShardRegistry::remove(const ShardId& id) {
        std::lock_guard<std::mutex> lk(_mutex);

        for (ShardMap::iterator i = _lookup.begin(); i != _lookup.end();) {
            shared_ptr<Shard> s = i->second;
            if (s->getId() == id) {
                _lookup.erase(i++);
            }
            else {
                ++i;
            }
        }

        for (ShardMap::iterator i = _rsLookup.begin(); i != _rsLookup.end();) {
            shared_ptr<Shard> s = i->second;
            if (s->getId() == id) {
                _rsLookup.erase(i++);
            }
            else {
                ++i;
            }
        }
    }

    void ShardRegistry::getAllShardIds(vector<ShardId>* all) const {
        std::set<string> seen;

        {
            std::lock_guard<std::mutex> lk(_mutex);
            for (ShardMap::const_iterator i = _lookup.begin(); i != _lookup.end(); ++i) {
                const shared_ptr<Shard>& s = i->second;
                if (s->getId() == "config") {
                    continue;
                }

                if (seen.count(s->getId())) {
                    continue;
                }
                seen.insert(s->getId());
            }
        }

        all->assign(seen.begin(), seen.end());
    }

    void ShardRegistry::toBSON(BSONObjBuilder* result) {
        BSONObjBuilder b(_lookup.size() + 50);

        std::lock_guard<std::mutex> lk(_mutex);

        for (ShardMap::const_iterator i = _lookup.begin(); i != _lookup.end(); ++i) {
            b.append(i->first, i->second->getConnString().toString());
        }

        result->append("map", b.obj());
    }

    void ShardRegistry::_addShard_inlock(const ShardType& shardType) {
        // This validation should ideally go inside the ShardType::validate call. However, doing
        // it there would prevent us from loading previously faulty shard hosts, which might have
        // been stored (i.e., the entire getAllShards call would fail).
        auto shardHostStatus = ConnectionString::parse(shardType.getHost());
        if (!shardHostStatus.isOK()) {
            warning() << "Unable to parse shard host "
                        << shardHostStatus.getStatus().toString();
        }

        const ConnectionString& shardHost(shardHostStatus.getValue());

        shared_ptr<Shard> shard = std::make_shared<Shard>(shardType.getName(), shardHost);
        _lookup[shardType.getName()] = shard;

        // Sync cluster connections (legacy config server) do not go through the normal targeting
        // mechanism and must only be reachable through CatalogManagerLegacy or legacy-style
        // queries and inserts. Do not create targeter for these connections. This code should go
        // away after 3.2 is released.
        if (shardHost.type() == ConnectionString::SYNC) {
            return;
        }

        // TODO: The only reason to have the shard host names in the lookup table is for the
        // setShardVersion call, which resolves the shard id from the shard address. This is
        // error-prone and will go away eventually when we switch all communications to go through
        // the remote command runner.
        _lookup[shardType.getHost()] = shard;

        for (const HostAndPort& hostAndPort : shardHost.getServers()) {
            _lookup[hostAndPort.toString()] = shard;

            // Maintain a mapping from host to shard it belongs to for the case where we need to
            // update the shard connection string on reconfigurations.
            if (shardHost.type() == ConnectionString::SET) {
                _rsLookup[hostAndPort.toString()] = shard;
            }
        }

        if (shardHost.type() == ConnectionString::SET) {
            _rsLookup[shardHost.getSetName()] = shard;
        }

        _targeters[shardType.getName()] = std::move(_targeterFactory->create(shardHost));
    }

    shared_ptr<Shard> ShardRegistry::_findUsingLookUp(const ShardId& shardId) {
        std::lock_guard<std::mutex> lk(_mutex);
        ShardMap::iterator it = _lookup.find(shardId);
        if (it != _lookup.end()) {
            return it->second;
        }

        return nullptr;
    }

    std::shared_ptr<RemoteCommandTargeter> ShardRegistry::_findTargeter(const string& shardId) {
        std::lock_guard<std::mutex> lk(_mutex);

        TargeterMap::iterator it = _targeters.find(shardId);
        if (it != _targeters.end()) {
            return it->second;
        }

        return nullptr;
    }

} // namespace mongo
