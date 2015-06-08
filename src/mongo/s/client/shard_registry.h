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

#include "mongo/s/client/shard.h"

namespace mongo {

    class BSONObjBuilder;
    class CatalogManager;
    class Shard;
    class ShardType;

    /**
     * Maintains the set of all shards known to the MongoS instance.
     */
    class ShardRegistry {
    public:
        ShardRegistry(CatalogManager* catalogManager);
        ~ShardRegistry();

        void reload();

        boost::shared_ptr<Shard> findIfExists(const ShardId& id);

        /**
         * Lookup shard by replica set name. Returns nullptr if the name can't be found.
         * Note: this doesn't refresh the table if the name isn't found, so it's possible that a
         * newly added shard/Replica Set may not be found.
         */
        ShardPtr lookupRSName(const std::string& name);

        void set(const ShardId& id, const Shard& s);

        void remove(const ShardId& id);

        void getAllShardIds(std::vector<ShardId>* all) const;

        bool isAShardNode(const std::string& addr) const;

        void toBSON(BSONObjBuilder* result) const;

    private:
        typedef std::map<ShardId, boost::shared_ptr<Shard>> ShardMap;


        /**
         * Creates a shard based on the specified information and puts it into the lookup maps.
         */
        void _addShard_inlock(const ShardType& shardType);

        boost::shared_ptr<Shard> _findUsingLookUp(const ShardId& id);

        // Catalog manager from which to load the shard information. Not owned and must outlive
        // the shard registry object.
        CatalogManager* const _catalogManager;

        // Protects the maps below
        mutable boost::mutex _mutex;

        // Map of both shardName -> Shard and hostName -> Shard
        ShardMap _lookup;

        // Map from ReplSet name to shard
        ShardMap _rsLookup;
    };

} // namespace mongo
