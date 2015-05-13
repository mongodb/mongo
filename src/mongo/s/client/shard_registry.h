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

#include <boost/shared_ptr.hpp>
#include <boost/thread/mutex.hpp>
#include <string>
#include <vector>

namespace mongo {

    class BSONObjBuilder;
    class CatalogManager;
    class Shard;


    /**
     * Maintains the set of all shards known to the MongoS instance.
     */
    class ShardRegistry {
    public:
        ShardRegistry(CatalogManager* catalogManager);
        ~ShardRegistry();

        void reload();

        boost::shared_ptr<Shard> findIfExists(const std::string& shardName);
        boost::shared_ptr<Shard> find(const std::string& ident);

        /**
         * Lookup shard by replica set name. Returns Shard::EMTPY if the name can't be found.
         * Note: this doesn't refresh the table if the name isn't found, so it's possible that a
         * newly added shard/Replica Set may not be found.
         */
        Shard lookupRSName(const std::string& name);

        /**
         * Useful for ensuring our shard data will not be modified while we use it.
         */
        Shard findCopy(const std::string& ident);

        void set(const std::string& name, const Shard& s);

        void remove(const std::string& name);

        void getAllShards(std::vector<Shard>& all) const;

        bool isAShardNode(const std::string& addr) const;

        void toBSON(BSONObjBuilder* result) const;


    private:
        typedef std::map<std::string, boost::shared_ptr<Shard>> ShardMap;


        boost::shared_ptr<Shard> _findWithRetry(const std::string& ident);

        boost::shared_ptr<Shard> _findUsingLookUp(const std::string& shardName);

        void _installHost(const std::string& host, const boost::shared_ptr<Shard>& s);


        // Catalog manager from which to load the shard information. Not owned and must outlive
        // the shard registry object.
        CatalogManager* const _catalogManager;

        // Map of both shardName -> Shard and hostName -> Shard
        mutable boost::mutex _mutex;
        ShardMap _lookup;

        // Map from ReplSet name to shard
        mutable boost::mutex _rsMutex;
        ShardMap _rsLookup;
    };

} // namespace mongo
