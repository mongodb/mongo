// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/ddl/internal_rename_if_options_and_indexes_match_gen.h"
#include "mongo/db/shard_role/shard_catalog/rename_collection.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/platform/compiler.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/str.h"

#include <list>
#include <memory>
#include <string>
#include <vector>

namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(blockBeforeInternalRenameAndBeforeTakingDDLLocks);

/**
 * Rename a collection while checking collection option and indexes.
 */
class InternalRenameIfOptionsAndIndexesMatchCmd final
    : public TypedCommand<InternalRenameIfOptionsAndIndexesMatchCmd> {
public:
    using Request = InternalRenameIfOptionsAndIndexesMatch;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            auto thisRequest = request();
            const auto& fromNss = thisRequest.getFrom();
            const auto& toNss = thisRequest.getTo();
            const auto& originalIndexes = thisRequest.getIndexes();
            const auto& collectionOptions = thisRequest.getCollectionOptions();

            if (MONGO_unlikely(blockBeforeInternalRenameAndBeforeTakingDDLLocks.shouldFail())) {
                blockBeforeInternalRenameAndBeforeTakingDDLLocks.pauseWhileSet();
            }

            if (serverGlobalParams.clusterRole.has(ClusterRole::None)) {
                // No need to acquire additional locks in a non-sharded environment
                _internalRun(opCtx, fromNss, toNss, originalIndexes, collectionOptions);
            } else {
                // Sharded environment. Run the _shardsvrRenameCollection command, which will use
                // the RenameCollectionCoordinator.
                RenameCollectionRequest renameCollectionRequest(toNss);
                renameCollectionRequest.setDropTarget(true);
                renameCollectionRequest.setExpectedIndexes(originalIndexes);
                renameCollectionRequest.setExpectedCollectionOptions(collectionOptions);
                // TODO: SERVER-81975 (PM-1931) remove this once we enable support for $out to
                // sharded collections.
                renameCollectionRequest.setTargetMustNotBeSharded(true);

                ShardsvrRenameCollection shardsvrRenameCollectionRequest(fromNss);
                shardsvrRenameCollectionRequest.setRenameCollectionRequest(renameCollectionRequest);

                // _shardsvrRenameCollection requires majority write concern.
                generic_argument_util::setMajorityWriteConcern(shardsvrRenameCollectionRequest);

                DBDirectClient client(opCtx);
                BSONObj cmdResult;
                client.runCommand(
                    fromNss.dbName(), shardsvrRenameCollectionRequest.toBSON(), cmdResult);
                uassertStatusOK(getStatusFromCommandResult(cmdResult));
            }
        }

    private:
        static void _internalRun(OperationContext* opCtx,
                                 const NamespaceString& fromNss,
                                 const NamespaceString& toNss,
                                 const std::vector<BSONObj>& indexList,
                                 const BSONObj& collectionOptions) {
            RenameCollectionOptions options;
            options.dropTarget = true;
            options.stayTemp = false;
            options.expectedIndexes = indexList;
            options.expectedCollectionOptions = collectionOptions;
            doLocalRenameIfOptionsAndIndexesHaveNotChanged(opCtx, fromNss, toNss, options);
        }

        NamespaceString ns() const override {
            return request().getFrom();
        }

        const DatabaseName& db() const override {
            return request().getDbName();
        }

        bool supportsWriteConcern() const override {
            return true;
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
        return "Internal command to rename and check collection options";
    }

    bool adminOnly() const override {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }
};
MONGO_REGISTER_COMMAND(InternalRenameIfOptionsAndIndexesMatchCmd).forShard();

}  // namespace
}  // namespace mongo
