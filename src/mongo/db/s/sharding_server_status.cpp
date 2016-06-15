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

#include "mongo/platform/basic.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/server_options.h"
#include "mongo/s/balancer/balancer.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"

namespace mongo {
namespace {

const char kBalancer[] = "balancer";

class ShardingServerStatus : public ServerStatusSection {
public:
    ShardingServerStatus() : ServerStatusSection("sharding") {}

    bool includeByDefault() const final {
        return true;
    }

    BSONObj generateSection(OperationContext* txn, const BSONElement& configElement) const final {
        BSONObjBuilder result;

        _reportShardingState(txn, &result);
        _reportBalancer(txn, &result);

        return result.obj();
    }

private:
    static void _reportShardingState(OperationContext* txn, BSONObjBuilder* builder) {
        ShardingState* const shardingState = ShardingState::get(txn);

        if (shardingState->enabled() &&
            serverGlobalParams.clusterRole != ClusterRole::ConfigServer) {
            builder->append("configsvrConnectionString",
                            shardingState->getConfigServer(txn).toString());

            Grid::get(txn)->configOpTime().append(builder, "lastSeenConfigServerOpTime");
        }
    }

    static void _reportBalancer(OperationContext* txn, BSONObjBuilder* builder) {
        Balancer* const balancer = Balancer::get(txn);
        if (balancer) {
            BSONObjBuilder balancer(builder->subobjStart(kBalancer));
            Balancer::get(txn)->report(&balancer);
            balancer.doneFast();
        }
    }

} shardingServerStatus;

}  // namespace
}  // namespace mongo
