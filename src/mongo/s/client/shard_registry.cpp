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
#include "mongo/s/client/shard.h"
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

        // We use the _lookup table for all shards and for the primary config DB. The config DB
        // info, however, does not come from the ShardNS::shard. So when cleaning the _lookup table
        // we leave the config state intact. The rationale is that this way we could drop shards
        // that were removed without reinitializing the config DB information.

        ShardMap::iterator i = _lookup.find("config");
        if (i != _lookup.end()) {
            shared_ptr<Shard> config = i->second;
            _lookup.clear();
            _lookup["config"] = config;
        }
        else {
            _lookup.clear();
        }

        _rsLookup.clear();

        for (const ShardType& shardData : shards) {
            uassertStatusOK(shardData.validate());

            shared_ptr<Shard> shard = boost::make_shared<Shard>(shardData.getName(),
                                                                shardData.getHost(),
                                                                shardData.getMaxSize(),
                                                                shardData.getDraining());
            _lookup[shardData.getName()] = shard;
            _lookup[shardData.getHost()] = shard;

            const ConnectionString& cs = shard->getAddress();

            if (cs.type() == ConnectionString::SET) {
                if (cs.getSetName().size()) {
                    boost::lock_guard<boost::mutex> lk(_rsMutex);
                    _rsLookup[cs.getSetName()] = shard;
                }

                vector<HostAndPort> servers = cs.getServers();
                for (unsigned i = 0; i < servers.size(); i++) {
                    _lookup[servers[i].toString()] = shard;
                }
            }
        }
    }

    shared_ptr<Shard> ShardRegistry::findIfExists(const string& shardName) {
        shared_ptr<Shard> shard = _findUsingLookUp(shardName);
        if (shard) {
            return shard;
        }

        // If we can't find the shard, we might just need to reload the cache
        reload();

        return _findUsingLookUp(shardName);
    }

    shared_ptr<Shard> ShardRegistry::find(const string& ident) {
        string errmsg;
        ConnectionString connStr = ConnectionString::parse(ident, errmsg);
        uassert(18642,
                str::stream() << "Error parsing connection string: " << ident,
                errmsg.empty());

        if (connStr.type() == ConnectionString::SET) {
            boost::lock_guard<boost::mutex> lk(_rsMutex);
            ShardMap::iterator iter = _rsLookup.find(connStr.getSetName());

            if (iter == _rsLookup.end()) {
                return nullptr;
            }

            return iter->second;
        }
        else {
            boost::lock_guard<boost::mutex> lk(_mutex);
            ShardMap::iterator iter = _lookup.find(ident);

            if (iter == _lookup.end()) {
                return nullptr;
            }

            return iter->second;
        }
    }

    Shard ShardRegistry::lookupRSName(const string& name) {
        boost::lock_guard<boost::mutex> lk(_rsMutex);
        ShardMap::iterator i = _rsLookup.find(name);

        return (i == _rsLookup.end()) ? Shard::EMPTY : *(i->second.get());
    }

    Shard ShardRegistry::findCopy(const string& ident){
        shared_ptr<Shard> found = _findWithRetry(ident);

        boost::lock_guard<boost::mutex> lk(_mutex);
        massert(13128, str::stream() << "can't find shard for: " << ident, found.get());

        return *found.get();
    }

    void ShardRegistry::set(const string& name, const Shard& s) {
        shared_ptr<Shard> ss(boost::make_shared<Shard>(s));

        boost::lock_guard<boost::mutex> lk(_mutex);
        _lookup[name] = ss;
    }

    void ShardRegistry::remove(const string& name) {
        boost::lock_guard<boost::mutex> lk(_mutex);

        for (ShardMap::iterator i = _lookup.begin(); i != _lookup.end();) {
            shared_ptr<Shard> s = i->second;
            if (s->getName() == name) {
                _lookup.erase(i++);
            }
            else {
                ++i;
            }
        }

        for (ShardMap::iterator i = _rsLookup.begin(); i != _rsLookup.end();) {
            shared_ptr<Shard> s = i->second;
            if (s->getName() == name) {
                _rsLookup.erase(i++);
            }
            else {
                ++i;
            }
        }
    }

    void ShardRegistry::getAllShards(vector<Shard>& all) const {
        std::set<string> seen;

        boost::lock_guard<boost::mutex> lk(_mutex);
        for (ShardMap::const_iterator i = _lookup.begin(); i != _lookup.end(); ++i) {
            const shared_ptr<Shard>& s = i->second;
            if (s->getName() == "config") {
                continue;
            }

            if (seen.count(s->getName())) {
                continue;
            }

            seen.insert(s->getName());
            all.push_back(*s);
        }
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
            b.append(i->first, i->second->getConnString());
        }

        result->append("map", b.obj());
    }

    shared_ptr<Shard> ShardRegistry::_findWithRetry(const string& ident) {
        shared_ptr<Shard> shard(find(ident));
        if (shard != nullptr) {
            return shard;
        }

        // Not in our maps, re-load all
        reload();

        shard = find(ident);
        massert(13129, str::stream() << "can't find shard for: " << ident, shard != NULL);

        return shard;
    }

    shared_ptr<Shard> ShardRegistry::_findUsingLookUp(const string& shardName) {
        boost::lock_guard<boost::mutex> lk(_mutex);
        ShardMap::iterator it = _lookup.find(shardName);

        if (it != _lookup.end()) {
            return it->second;
        }

        return nullptr;
    }

} // namespace mongo
