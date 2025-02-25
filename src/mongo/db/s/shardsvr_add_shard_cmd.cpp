/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */


#include <memory>
#include <string>


#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/cluster_role.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/repl_set_config.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/topology_change_helpers.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/s/balancer_configuration.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/add_shard_gen.h"
#include "mongo/util/assert_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

/**
 * Internal sharding command run on mongod to initialize itself as a shard in the cluster.
 */
class ShardsvrAddShardCommand : public TypedCommand<ShardsvrAddShardCommand> {
public:
    using Request = ShardsvrAddShard;

    ShardsvrAddShardCommand() : TypedCommand(Request::kCommandName, Request::kCommandAlias) {}

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            uassert(50876,
                    "Cannot run addShard on a node started without --shardsvr",
                    serverGlobalParams.clusterRole.has(ClusterRole::ShardServer));
            tassert(5624104,
                    "Cannot run addShard on a node that contains customized getLastErrorDefaults, "
                    "which has been deprecated and is now ignored. Use setDefaultRWConcern instead "
                    "to set a cluster-wide default writeConcern.",
                    !repl::ReplicationCoordinator::get(opCtx)
                         ->getConfig()
                         .containsCustomizedGetLastErrorDefaults());

            auto addShardCmd = request();

            // A request dispatched through a local client is served within the same thread that
            // submits it (so that the opCtx needs to be used as the vehicle to pass the WC to the
            // ServiceEntryPoint).
            const auto originalWC = opCtx->getWriteConcern();
            ScopeGuard resetWCGuard([&] { opCtx->setWriteConcern(originalWC); });
            opCtx->setWriteConcern(ShardingCatalogClient::kMajorityWriteConcern);

            BSONObj newIdentity = addShardCmd.getShardIdentity().toBSON().addFields(
                BSON("_id" << add_shard_util::kShardIdentityDocumentId));

            write_ops::InsertCommandRequest insertOp(
                NamespaceString::kServerConfigurationNamespace);
            insertOp.setDocuments({newIdentity});
            BSONObjBuilder cmdObjBuilder;
            insertOp.serialize(&cmdObjBuilder);
            cmdObjBuilder.append(WriteConcernOptions::kWriteConcernField,
                                 ShardingCatalogClient::kMajorityWriteConcern.toBSON());

            DBDirectClient localClient(opCtx);

            BSONObj res;
            localClient.runCommand(DatabaseName::kAdmin, cmdObjBuilder.obj(), res);
            const auto status = getStatusFromWriteCommandReply(res);
            if (status.code() != ErrorCodes::DuplicateKey) {
                uassertStatusOK(status);
                return;
            }

            // If we have a duplicate key, that means, we already have a shardIdentity document. If
            // so, we only allow it to be the very same as the one in the command, otherwise a
            // cluster could steal an other cluster's shard without the former knowing it.
            const auto existingIdentity =
                localClient.findOne(NamespaceString::kServerConfigurationNamespace,
                                    BSON("_id" << add_shard_util::kShardIdentityDocumentId));

            invariant(!existingIdentity.isEmpty());

            uassert(ErrorCodes::IllegalOperation,
                    "Shard already has an identity that differs",
                    newIdentity.woCompare(existingIdentity,
                                          {},
                                          BSONObj::ComparisonRules::kConsiderFieldName |
                                              BSONObj::ComparisonRules::kIgnoreFieldOrder) == 0);

            const auto balancerConfig = Grid::get(opCtx)->getBalancerConfiguration();
            invariant(balancerConfig);
            // Ensure we have the most up-to-date balancer configuration
            uassertStatusOK(balancerConfig->refreshAndCheck(opCtx));

            localClient.update(NamespaceString::kServerConfigurationNamespace,
                               BSON("_id"
                                    << "AddShardStats"),
                               BSON("$inc" << BSON("count" << 1)),
                               true /* upsert */,
                               false /* multi */);
        }

    private:
        bool supportsWriteConcern() const override {
            return false;
        }

        // The command parameter happens to be string so it's historically been interpreted
        // by parseNs as a collection. Continuing to do so here for unexamined compatibility.
        NamespaceString ns() const override {
            return NamespaceString(request().getDbName());
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(
                            ResourcePattern::forClusterResource(request().getDbName().tenantId()),
                            ActionType::internal));
        }
    };

    bool skipApiVersionCheck() const override {
        // Internal command (server to server).
        return true;
    }

    std::string help() const override {
        return "Internal command, which is exported by shards. Do not call "
               "directly. Adds a new shard to a cluster.";
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const override {
        return true;
    }
};
MONGO_REGISTER_COMMAND(ShardsvrAddShardCommand).forShard();

}  // namespace
}  // namespace mongo
