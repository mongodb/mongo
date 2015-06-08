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

#include <boost/make_shared.hpp>
#include <boost/thread/lock_guard.hpp>

#include "mongo/client/connection_string.h"
#include "mongo/s/catalog/catalog_manager.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    using boost::shared_ptr;
    using std::string;
    using std::vector;


    ShardRegistry::ShardRegistry(CatalogManager* catalogManager)
        : _catalogManager(catalogManager) {

    }

    ShardRegistry::~ShardRegistry() = default;

    void ShardRegistry::reload() {
        vector<ShardType> shards;
        Status status = _catalogManager->getAllShards(&shards);
        massert(13632, "couldn't get updated shard list from config server", status.isOK());

        int numShards = shards.size();

        LOG(1) << "found " << numShards << " shards listed on config server(s)";

        boost::lock_guard<boost::mutex> lk(_mutex);

        _lookup.clear();
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

    shared_ptr<Shard> ShardRegistry::lookupRSName(const string& name) {
        boost::lock_guard<boost::mutex> lk(_mutex);
        ShardMap::iterator i = _rsLookup.find(name);

        return (i == _rsLookup.end()) ? nullptr : i->second;
    }

    void ShardRegistry::set(const ShardId& id, const Shard& s) {
        shared_ptr<Shard> ss(boost::make_shared<Shard>(s.getId(),
                                                       s.getConnString(),
                                                       s.getMaxSizeMB(),
                                                       s.isDraining()));

        boost::lock_guard<boost::mutex> lk(_mutex);
        _lookup[id] = ss;
    }

    void ShardRegistry::remove(const ShardId& id) {
        boost::lock_guard<boost::mutex> lk(_mutex);

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
            boost::lock_guard<boost::mutex> lk(_mutex);
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

    bool ShardRegistry::isAShardNode(const string& addr) const {
        boost::lock_guard<boost::mutex> lk(_mutex);

        // Check direct nods or set names
        ShardMap::const_iterator i = _lookup.find(addr);
        if (i != _lookup.end()) {
            return true;
        }

        // Check for set nodes
        for (ShardMap::const_iterator i = _lookup.begin(); i != _lookup.end(); ++i) {
            if (i->first == "config") {
                continue;
            }

            if (i->second->containsNode(addr)) {
                return true;
            }
        }

        return false;
    }

    void ShardRegistry::toBSON(BSONObjBuilder* result) const {
        BSONObjBuilder b(_lookup.size() + 50);

        boost::lock_guard<boost::mutex> lk(_mutex);

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

        shared_ptr<Shard> shard = boost::make_shared<Shard>(shardType.getName(),
                                                            shardHost,
                                                            shardType.getMaxSize(),
                                                            shardType.getDraining());
        _lookup[shardType.getName()] = shard;
        _lookup[shardType.getHost()] = shard;

        if (shardHost.type() == ConnectionString::SET) {
            if (shardHost.getSetName().size()) {
                _rsLookup[shardHost.getSetName()] = shard;
            }

            vector<HostAndPort> servers = shardHost.getServers();
            for (unsigned i = 0; i < servers.size(); i++) {
                _lookup[servers[i].toString()] = shard;
            }
        }
    }

    shared_ptr<Shard> ShardRegistry::_findUsingLookUp(const ShardId& id) {
        boost::lock_guard<boost::mutex> lk(_mutex);
        ShardMap::iterator it = _lookup.find(id);

        if (it != _lookup.end()) {
            return it->second;
        }

        return nullptr;
    }

} // namespace mongo
