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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/rename_collection.h"
#include "mongo/db/commands.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/active_rename_collection_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/rename_collection_gen.h"
#include "mongo/util/log.h"

namespace mongo {

MONGO_FAIL_POINT_DEFINE(hangRenameCollectionAfterGettingRename);

namespace {
/**
 * Internal sharding command run on a primary shard server to rename a collection.
 */
class ShardSvrRenameCollectionCommand final : public TypedCommand<ShardSvrRenameCollectionCommand> {
public:
    using Request = ShardsvrRenameCollection;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            auto incomingRequest = request();
            auto sourceCollUUID = request().getUuid();
            auto nssFromUUID = CollectionCatalog::get(opCtx).lookupNSSByUUID(sourceCollUUID);
            if (nssFromUUID == incomingRequest.getTo()) {
                repl::ReplClientInfo::forClient(opCtx->getClient())
                    .setLastOpToSystemLastOpTime(opCtx);
                return;
            }
            auto scopedRenameCollection =
                uassertStatusOK(ActiveRenameCollectionRegistry::get(opCtx).registerRenameCollection(
                    incomingRequest));

            if (MONGO_FAIL_POINT(hangRenameCollectionAfterGettingRename)) {
                log() << "Hit hangRenameCollectionAfterGettingRename";
                MONGO_FAIL_POINT_PAUSE_WHILE_SET_OR_INTERRUPTED(
                    opCtx, hangRenameCollectionAfterGettingRename);
            }

            // Check if there is an existing renameCollection running and if so, join it
            if (!scopedRenameCollection.mustExecute()) {
                scopedRenameCollection.awaitExecution().get();
            } else {
                try {
                    validateAndRunRenameCollection(opCtx,
                                                   ns(),
                                                   incomingRequest.getTo(),
                                                   incomingRequest.getDropTarget(),
                                                   incomingRequest.getStayTemp());
                } catch (const DBException& e) {
                    scopedRenameCollection.emplaceStatus(e.toStatus());
                    throw;
                } catch (const std::exception& e) {
                    scopedRenameCollection.emplaceStatus(
                        {ErrorCodes::InternalError,
                         str::stream()
                             << "Severe error occurred while running shardCollection command: "
                             << e.what()});
                    throw;
                }
                scopedRenameCollection.emplaceStatus(Status::OK());
            }
        }

    private:
        NamespaceString ns() const override {
            return request().getRenameCollection();
        }

        bool supportsWriteConcern() const override {
            return true;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(ResourcePattern::forClusterResource(),
                                                           ActionType::internal));
        }
    };

    std::string help() const override {
        return "Internal command, which is exported by shards."
               "Renames a collection.";
    }

    bool adminOnly() const override {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }
} shardSvrRenameCollectionCmd;

}  // namespace
}  // namespace mongo
