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
#include "mongo/s/client/shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"

namespace mongo {

    using std::string;

    Status DBClientShardResolver::chooseWriteHost(const string& shardName,
                                                  ConnectionString* shardHost) const {

        // Internally uses our shard cache, does no reload
        boost::shared_ptr<Shard> shard = grid.shardRegistry()->findIfExists(shardName);
        if (!shard) {
            return Status(ErrorCodes::ShardNotFound,
                          str::stream() << "unknown shard name " << shardName);
        }

        return findMaster(shard->getConnString(), shardHost);
    }

    Status DBClientShardResolver::findMaster(const ConnectionString& connString,
                                             ConnectionString* resolvedHost) {

        if (connString.type() == ConnectionString::MASTER) {
            *resolvedHost = connString;
            return Status::OK();
        }

        dassert(connString.type() == ConnectionString::SET);

        //
        // If we need to, then get the particular node we're targeting in the replica set
        //

        // Don't create the monitor unless we need to - fast path
        ReplicaSetMonitorPtr replMonitor = ReplicaSetMonitor::get(connString.getSetName());

        if (!replMonitor) {
            // Slow path
            std::set<HostAndPort> seedServers(connString.getServers().begin(),
                                              connString.getServers().end());
            ReplicaSetMonitor::createIfNeeded(connString.getSetName(), seedServers);

            replMonitor = ReplicaSetMonitor::get(connString.getSetName());
        }

        if (!replMonitor) {
            return Status(ErrorCodes::ReplicaSetNotFound,
                          str::stream() << "unknown replica set " << connString.getSetName());
        }

        try {
            // This can throw when we don't find a master!
            HostAndPort masterHostAndPort = replMonitor->getMasterOrUassert();
            *resolvedHost = fassertStatusOK(0, ConnectionString::parse(masterHostAndPort.toString()));
            return Status::OK();
        }
        catch ( const DBException& ) {
            return Status( ErrorCodes::HostNotFound,
                           string("could not contact primary for replica set ")
                           + replMonitor->getName() );
        }

        MONGO_UNREACHABLE;
    }

} // namespace mongo

