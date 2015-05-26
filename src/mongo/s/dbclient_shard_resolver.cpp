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

#include "mongo/s/dbclient_shard_resolver.h"

#include <set>

#include "mongo/client/replica_set_monitor.h"
#include "mongo/s/config.h"
#include "mongo/s/client/shard.h"

namespace mongo {

    using std::string;

    Status DBClientShardResolver::chooseWriteHost( const string& shardName,
                                                   ConnectionString* shardHost ) const {

        // Declare up here for parsing later
        string errMsg;

        // Special-case for config
        if (shardName == "config") {
            *shardHost = ConnectionString::parse( configServer.modelServer(), errMsg );
            dassert( errMsg == "" );
            return Status::OK();
        }

        //
        // First get the information about the shard from the shard cache
        //

        // Internally uses our shard cache, does no reload
        Shard shard = Shard::findIfExists( shardName );
        if ( shard.getName() == "" ) {
            return Status( ErrorCodes::ShardNotFound,
                           string("unknown shard name ") + shardName );
        }
        return findMaster(shard.getConnString().toString(), shardHost);
    }

    Status DBClientShardResolver::findMaster( const std::string connString,
                                              ConnectionString* resolvedHost ) {
        std::string errMsg;

        ConnectionString rawHost = ConnectionString::parse( connString, errMsg );
        dassert( errMsg == "" );
        dassert( rawHost.type() == ConnectionString::SET
                 || rawHost.type() == ConnectionString::MASTER );

        if ( rawHost.type() == ConnectionString::MASTER ) {
            *resolvedHost = rawHost;
            return Status::OK();
        }

        //
        // If we need to, then get the particular node we're targeting in the replica set
        //

        // Don't create the monitor unless we need to - fast path
        ReplicaSetMonitorPtr replMonitor = ReplicaSetMonitor::get(rawHost.getSetName());

        if (!replMonitor) {
            // Slow path
            std::set<HostAndPort> seedServers(rawHost.getServers().begin(),
                                              rawHost.getServers().end());
            ReplicaSetMonitor::createIfNeeded(rawHost.getSetName(), seedServers);
            replMonitor = ReplicaSetMonitor::get(rawHost.getSetName());
        }

        if (!replMonitor) {
            return Status( ErrorCodes::ReplicaSetNotFound,
                           string("unknown replica set ") + rawHost.getSetName() );
        }

        try {
            // This can throw when we don't find a master!
            HostAndPort masterHostAndPort = replMonitor->getMasterOrUassert();
            *resolvedHost = ConnectionString::parse( masterHostAndPort.toString(), errMsg );
            dassert( errMsg == "" );
            return Status::OK();
        }
        catch ( const DBException& ) {
            return Status( ErrorCodes::HostNotFound,
                           string("could not contact primary for replica set ")
                           + replMonitor->getName() );
        }

        // Unreachable
        dassert( false );
        return Status( ErrorCodes::UnknownError, "" );
    }

} // namespace mongo

