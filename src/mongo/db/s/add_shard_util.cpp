/**
 * Copyright (C) 2018 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/s/add_shard_util.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/db/repl/repl_set_config.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/add_shard_cmd_gen.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/cluster_identity_loader.h"
#include "mongo/s/shard_id.h"
#include "mongo/s/write_ops/batched_command_request.h"

namespace mongo {
namespace add_shard_util {

AddShard createAddShardCmd(OperationContext* opCtx, const ShardId& shardName) {
    AddShard addShardCmd;
    addShardCmd.setDbName(NamespaceString::kAdminDb);

    ShardIdentity shardIdentity;
    shardIdentity.setShardName(shardName.toString());
    shardIdentity.setClusterId(ClusterIdentityLoader::get(opCtx)->getClusterId());
    shardIdentity.setConfigsvrConnectionString(
        repl::ReplicationCoordinator::get(opCtx)->getConfig().getConnectionString());

    addShardCmd.setShardIdentity(shardIdentity);
    return addShardCmd;
}

BSONObj createShardIdentityUpsertForAddShard(const AddShard& addShardCmd) {
    BatchedCommandRequest request([&] {
        write_ops::Update updateOp(NamespaceString::kServerConfigurationNamespace);
        updateOp.setUpdates({[&] {
            write_ops::UpdateOpEntry entry;
            entry.setQ(BSON("_id" << kShardIdentityDocumentId));
            entry.setU(addShardCmd.getShardIdentity().toBSON());
            entry.setUpsert(true);
            return entry;
        }()});

        return updateOp;
    }());
    request.setWriteConcern(ShardingCatalogClient::kMajorityWriteConcern.toBSON());

    return request.toBSON();
}

}  // namespace mongo
}  // namespace add_shard_util
