/*
 *    Copyright (C) 2015 BongoDB Inc.
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

#include "bongo/platform/basic.h"

#include <string>
#include <vector>

#include "bongo/bson/bsonobjbuilder.h"
#include "bongo/client/connpool.h"
#include "bongo/client/global_conn_pool.h"
#include "bongo/db/commands.h"
#include "bongo/db/repl/replication_coordinator.h"
#include "bongo/db/repl/replication_coordinator_global.h"
#include "bongo/db/s/sharding_state.h"
#include "bongo/db/server_options.h"
#include "bongo/executor/connection_pool_stats.h"
#include "bongo/executor/network_interface_factory.h"
#include "bongo/executor/task_executor_pool.h"
#include "bongo/s/catalog/sharding_catalog_manager.h"
#include "bongo/s/client/shard_registry.h"
#include "bongo/s/grid.h"

namespace bongo {

class PoolStats final : public Command {
public:
    PoolStats() : Command("connPoolStats") {}

    void help(std::stringstream& help) const override {
        help << "stats about connections between servers in a replica set or sharded cluster.";
    }


    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    void addRequiredPrivileges(const std::string& dbname,
                               const BSONObj& cmdObj,
                               std::vector<Privilege>* out) override {
        ActionSet actions;
        actions.addAction(ActionType::connPoolStats);
        out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
    }

    bool run(OperationContext* txn,
             const std::string&,
             bongo::BSONObj&,
             int,
             std::string&,
             bongo::BSONObjBuilder& result) override {
        executor::ConnectionPoolStats stats{};

        // Global connection pool connections.
        globalConnPool.appendConnectionStats(&stats);
        result.appendNumber("numClientConnections", DBClientConnection::getNumConnections());
        result.appendNumber("numAScopedConnections", AScopedConnection::getNumConnections());

        // Replication connections, if we have them.
        auto replCoord = repl::ReplicationCoordinator::get(txn);
        if (replCoord && replCoord->isReplEnabled()) {
            replCoord->appendConnectionStats(&stats);
        }

        // Sharding connections, if we have any.
        auto grid = Grid::get(txn);
        if (grid->shardRegistry()) {
            grid->getExecutorPool()->appendConnectionStats(&stats);
            if (serverGlobalParams.clusterRole == ClusterRole::ConfigServer) {
                grid->catalogManager()->appendConnectionStats(&stats);
            }
        }

        // Output to a BSON object.
        stats.appendToBSON(result);

        // Always report all replica sets being tracked.
        BSONObjBuilder setStats(result.subobjStart("replicaSets"));
        globalRSMonitorManager.report(&setStats);
        setStats.doneFast();

        return true;
    }

    bool slaveOk() const override {
        return true;
    }

} poolStatsCmd;

}  // namespace bongo
