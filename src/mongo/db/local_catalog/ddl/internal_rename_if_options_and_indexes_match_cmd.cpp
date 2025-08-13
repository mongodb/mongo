/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/local_catalog/ddl/internal_rename_if_options_and_indexes_match_gen.h"
#include "mongo/db/local_catalog/rename_collection.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
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
            const auto indexList =
                std::list<BSONObj>(originalIndexes.begin(), originalIndexes.end());
            const auto& collectionOptions = thisRequest.getCollectionOptions();

            if (MONGO_unlikely(blockBeforeInternalRenameAndBeforeTakingDDLLocks.shouldFail())) {
                blockBeforeInternalRenameAndBeforeTakingDDLLocks.pauseWhileSet();
            }

            if (serverGlobalParams.clusterRole.has(ClusterRole::None)) {
                // No need to acquire additional locks in a non-sharded environment
                _internalRun(opCtx, fromNss, toNss, indexList, collectionOptions);
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

                // _shardsvrRenameCollecion requires majority write concern.
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
                                 const std::list<BSONObj>& indexList,
                                 const BSONObj& collectionOptions) {
            RenameCollectionOptions options;
            options.dropTarget = true;
            options.stayTemp = false;
            options.originalIndexes = indexList;
            options.originalCollectionOptions = collectionOptions;
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
