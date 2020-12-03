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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/rename_collection_common.h"
#include "mongo/db/commands/rename_collection_gen.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/sharded_ddl_commands_gen.h"

namespace mongo {
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
                    str::stream() << "Invalid source namespace: " << fromNss.ns(),
                    fromNss.isValid());

            uassert(ErrorCodes::InvalidNamespace,
                    str::stream() << "Invalid target namespace: " << toNss.ns(),
                    toNss.isValid());

            ShardsvrRenameCollection renameCollRequest(fromNss);
            renameCollRequest.setDbName(fromNss.db());
            renameCollRequest.setDropTarget(request().getDropTarget());
            renameCollRequest.setStayTemp(request().getStayTemp());
            renameCollRequest.setTo(request().getTo());

            auto catalogCache = Grid::get(opCtx)->catalogCache();
            const auto dbInfo = uassertStatusOK(catalogCache->getDatabase(opCtx, fromNss.db()));
            auto cri = uassertStatusOK(catalogCache->getCollectionRoutingInfo(opCtx, fromNss));

            auto shard = uassertStatusOK(
                Grid::get(opCtx)->shardRegistry()->getShard(opCtx, dbInfo.primaryId()));

            auto cmdResponse = uassertStatusOK(shard->runCommandWithFixedRetryAttempts(
                opCtx,
                ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                fromNss.db().toString(),
                applyReadWriteConcern(opCtx,
                                      this,
                                      appendDbVersionIfPresent(renameCollRequest.toBSON({}),
                                                               dbInfo.databaseVersion())),
                Shard::RetryPolicy::kNoRetry));

            uassertStatusOK(cmdResponse.commandStatus);

            auto renameCollResp = RenameCollectionResponse::parse(
                IDLParserErrorContext("renameCollection"), cmdResponse.response);

            // TODO: SERVER-53098 advance the cache by collection version.
            catalogCache->invalidateShardOrEntireCollectionEntryForShardedCollection(
                toNss, renameCollResp.getCollectionVersion(), dbInfo.primaryId());

            catalogCache->invalidateCollectionEntry_LINEARIZABLE(fromNss);
        }

        NamespaceString ns() const override {
            return request().getCommandParameter();
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassertStatusOK(rename_collection::checkAuthForRenameCollectionCommand(
                opCtx->getClient(), ns().db().toString(), request().toBSON(BSONObj())));
        }

        bool supportsWriteConcern() const override {
            return true;
        }
    };

} renameCollectionCmd;

}  // namespace
}  // namespace mongo