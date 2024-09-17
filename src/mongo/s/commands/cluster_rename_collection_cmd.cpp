/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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
#include <variant>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/catalog/collection_uuid_mismatch_info.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/rename_collection_common.h"
#include "mongo/db/commands/rename_collection_gen.h"
#include "mongo/db/curop_failpoint_helpers.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/s/catalog/type_database_gen.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/cluster_ddl.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/sharded_ddl_commands_gen.h"
#include "mongo/s/shard_version.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/read_through_cache.h"
#include "mongo/util/str.h"
#include "mongo/util/uuid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
MONGO_FAIL_POINT_DEFINE(renameWaitAfterDatabaseCreation);
namespace {

class RenameCollectionCmd final : public TypedCommand<RenameCollectionCmd> {
public:
    using Request = RenameCollectionCommand;

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const override {
        return true;
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            auto fromNss = ns();
            auto toNss = request().getTo();

            uassert(ErrorCodes::InvalidNamespace,
                    str::stream() << "Invalid source namespace: " << fromNss.toStringForErrorMsg(),
                    fromNss.isValid());

            uassert(ErrorCodes::InvalidNamespace,
                    str::stream() << "Invalid target namespace: " << toNss.toStringForErrorMsg(),
                    toNss.isValid());

            uassert(ErrorCodes::IllegalOperation,
                    "Can't rename a collection to itself",
                    fromNss != toNss);

            if (fromNss.isTimeseriesBucketsCollection()) {
                uassert(ErrorCodes::IllegalOperation,
                        "Renaming system.buckets collections is not allowed",
                        AuthorizationSession::get(opCtx->getClient())
                            ->isAuthorizedForActionsOnResource(
                                ResourcePattern::forClusterResource(fromNss.tenantId()),
                                ActionType::setUserWriteBlockMode));
            }

            RenameCollectionRequest renameCollReq(request().getTo());
            renameCollReq.setStayTemp(request().getStayTemp());
            renameCollReq.setExpectedSourceUUID(request().getCollectionUUID());
            visit(
                OverloadedVisitor{
                    [&renameCollReq](bool dropTarget) { renameCollReq.setDropTarget(dropTarget); },
                    [&renameCollReq](const UUID& uuid) {
                        renameCollReq.setDropTarget(true);
                        renameCollReq.setExpectedTargetUUID(uuid);
                    },
                },
                request().getDropTarget());

            ShardsvrRenameCollection renameCollRequest(fromNss);
            renameCollRequest.setDbName(fromNss.dbName());
            renameCollRequest.setRenameCollectionRequest(renameCollReq);
            renameCollRequest.setAllowEncryptedCollectionRename(
                AuthorizationSession::get(opCtx->getClient())
                    ->isAuthorizedForActionsOnResource(
                        ResourcePattern::forClusterResource(fromNss.tenantId()),
                        ActionType::setUserWriteBlockMode));

            auto catalogCache = Grid::get(opCtx)->catalogCache();
            auto swDbInfo = Grid::get(opCtx)->catalogCache()->getDatabase(opCtx, fromNss.dbName());
            if (swDbInfo == ErrorCodes::NamespaceNotFound) {
                uassert(CollectionUUIDMismatchInfo(fromNss.dbName(),
                                                   *request().getCollectionUUID(),
                                                   fromNss.coll().toString(),
                                                   boost::none),
                        "Database does not exist",
                        !request().getCollectionUUID());
            }
            const auto dbInfo = uassertStatusOK(swDbInfo);

            auto shard = uassertStatusOK(
                Grid::get(opCtx)->shardRegistry()->getShard(opCtx, dbInfo->getPrimary()));

            // Creates the destination database if it doesn't exist already.
            cluster::createDatabase(opCtx, toNss.dbName(), shard->getId());

            CurOpFailpointHelpers::waitWhileFailPointEnabled(
                &renameWaitAfterDatabaseCreation, opCtx, "renameWaitAfterDatabaseCreation", []() {
                    LOGV2(8433001,
                          "Hanging rename due to 'renameWaitAfterDatabaseCreation' "
                          "failpoint");
                });

            generic_argument_util::setMajorityWriteConcern(renameCollRequest);
            generic_argument_util::setDbVersionIfPresent(renameCollRequest, dbInfo->getVersion());

            auto cmdResponse = uassertStatusOK(shard->runCommandWithFixedRetryAttempts(
                opCtx,
                ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                fromNss.dbName(),
                renameCollRequest.toBSON(),
                Shard::RetryPolicy::kNoRetry));

            uassertStatusOK(cmdResponse.commandStatus);

            auto renameCollResp = RenameCollectionResponse::parse(
                IDLParserContext("renameCollection"), cmdResponse.response);

            catalogCache->invalidateShardOrEntireCollectionEntryForShardedCollection(
                toNss, renameCollResp.getCollectionVersion(), dbInfo->getPrimary());

            catalogCache->invalidateCollectionEntry_LINEARIZABLE(fromNss);
            catalogCache->invalidateIndexEntry_LINEARIZABLE(fromNss);
        }

        NamespaceString ns() const override {
            return request().getCommandParameter();
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassertStatusOK(rename_collection::checkAuthForRenameCollectionCommand(
                opCtx->getClient(), request()));
        }

        bool supportsWriteConcern() const override {
            return true;
        }
    };
};
MONGO_REGISTER_COMMAND(RenameCollectionCmd).forRouter();

}  // namespace
}  // namespace mongo
