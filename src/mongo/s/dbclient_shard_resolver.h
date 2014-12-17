/**
 *    Copyright (C) 2013 MongoDB Inc.
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

#include "mongo/s/shard_resolver.h"

namespace mongo {

    /**
     * ShardResolver based on the Shard and ReplicaSetMonitor caches.
     *
     * TODO: Currently it's possible for the shard resolver to be stale after we target and remove
     * a shard.  We need to figure out how to refresh.
     */
    class DBClientShardResolver : public ShardResolver {
    public:

        DBClientShardResolver() {
        }

        virtual ~DBClientShardResolver() {
        }

        /**
         * Returns the current host ConnectionString for a write to a shard.
         *
         * Note: Does *not* trigger a refresh of either the shard or replica set monitor caches,
         * though refreshes may happen unexpectedly between calls.
         *
         * Returns ShardNotFound if the shard name is unknown
         * Returns ReplicaSetNotFound if the replica set is not being tracked
         * Returns !OK with message if the shard host could not be found for other reasons.
         */
        Status chooseWriteHost( const std::string& shardName, ConnectionString* shardHost ) const;
        
        /**
         *  Resolves a replica set connection std::string to a master, if possible.
         * Returns HostNotFound if the master is not reachable
         * Returns ReplicaSetNotFound if the replica set is not being tracked
         */
        static Status findMaster( const std::string connString, ConnectionString* resolvedHost );

    };

} // namespace mongo
