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


#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/cancelable_operation_context.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/global_catalog/sharding_catalog_client.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/repl_set_config.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/topology/add_shard_gen.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/db/topology/topology_change_helpers.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/s/balancer_configuration.h"
#include "mongo/util/assert_util.h"

#include <memory>
#include <string>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

void writeNoopEntryLocal(OperationContext* opCtx) {
    DBDirectClient client(opCtx);
    client.update(NamespaceString::kServerConfigurationNamespace,
                  BSON("_id" << "AddShardStats"),
                  BSON("$inc" << BSON("count" << 1)),
                  true /* upsert */,
                  false /* multi */);
}

void waitForMajority(OperationContext* opCtx) {
    const auto majorityWriteStatus =
        WaitForMajorityService::get(opCtx->getServiceContext())
            .waitUntilMajorityForWrite(repl::ReplicationCoordinator::get(opCtx->getServiceContext())
                                           ->getMyLastAppliedOpTime(),
                                       opCtx->getCancellationToken())
            .getNoThrow();

    if (majorityWriteStatus == ErrorCodes::CallbackCanceled) {
        uassertStatusOK(opCtx->checkForInterruptNoAssert());
    }
    uassertStatusOK(majorityWriteStatus);
}

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

            // TODO (SERVER-97816): uassert that OSI has been attached once 9.0 becomes last LTS.
            const auto txnParticipant = TransactionParticipant::get(opCtx);

            auto addShardCmd = request();

            // First write the shard identity with local write concern - this can be done even with
            // a session checked out since there are no nested commands which may try to check out
            // another session. Doing this instantiates the grid locally which allows us to wait
            // for majority on an AlternativeClientRegion without having to create a separate
            // executor.
            bool wroteSomething = topology_change_helpers::installShardIdentity(
                opCtx, addShardCmd.getShardIdentity());

            // If the write of the shard identity document didn't actually write anything (the
            // document already existed) then we need to do a noop write so that we have something
            // to wait for to ensure the document was majority committed. We do this now so that we
            // know the write of the shard identity has been applied before we use the Grid in the
            // following operations.
            if (!wroteSomething) {
                writeNoopEntryLocal(opCtx);
            }

            // Now we need to wait for majority. If this is the coordinator path and we have session
            // info, we need an alternative client region so that we don't wait for majority with
            // a session checked out.
            // TODO (SERVER - 97816): remove non - OSI path once 9.0 becomes last LTS.
            if (txnParticipant) {
                auto newClient = getGlobalServiceContext()
                                     ->getService(ClusterRole::ShardServer)
                                     ->makeClient("ShardsvrAddShard");
                AlternativeClientRegion acr(newClient);
                auto cancelableOperationContext = CancelableOperationContext(
                    cc().makeOperationContext(),
                    opCtx->getCancellationToken(),
                    Grid::get(opCtx)->getExecutorPool()->getFixedExecutor());
                cancelableOperationContext->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

                waitForMajority(cancelableOperationContext.get());
            } else {
                waitForMajority(opCtx);
            }

            const auto balancerConfig = Grid::get(opCtx)->getBalancerConfiguration();
            invariant(balancerConfig);
            // Ensure we have the most up-to-date balancer configuration
            uassertStatusOK(balancerConfig->refreshAndCheck(opCtx));

            // Since we know that some above write (either the shard identity or the noop write) was
            // done with the session info, there is no need to do another noop write here (in fact,
            // it would not write anything since the transaction number has already been used). It
            // is ok that the write is not the last operation in the command because if any error
            // occurs we will retry with a higher transcation number an execute the whole command
            // again anyways.
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

    bool supportsRetryableWrite() const final {
        return true;
    }

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
