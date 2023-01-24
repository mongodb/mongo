/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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


#include "mongo/db/commands.h"
#include "mongo/db/metadata_consistency_types_gen.h"
#include "mongo/s/check_metadata_consistency_gen.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/grid.h"
#include "mongo/s/query/store_possible_cursor.h"
#include "mongo/s/request_types/sharded_ddl_commands_gen.h"
#include "mongo/s/sharding_feature_flags_gen.h"


namespace mongo {
namespace {

bool isClusterLevelModeCommand(const NamespaceString& nss) {
    return nss.isAdminDB();
}

bool isCollectionLevelModeCommand(const NamespaceString& nss) {
    return !nss.isAdminDB() && !nss.isCollectionlessCursorNamespace();
}

class CheckMetadataConsistencyCmd final : public TypedCommand<CheckMetadataConsistencyCmd> {
public:
    using Request = CheckMetadataConsistency;
    using Response = CheckMetadataConsistencyResponse;

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const override {
        return false;
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        Response typedRun(OperationContext* opCtx) {
            CommandHelpers::handleMarkKillOnClientDisconnect(opCtx);

            const auto& nss = ns();

            // Cluster and Collection level mode command is not implemented
            //  - db.adminCommand({checkMetadataConsistency: 1})
            //  - db.runCommand({checkMetadataConsistency: "coll"})
            uassert(ErrorCodes::NotImplemented,
                    "cluster and collection level mode command is not implemented",
                    !isClusterLevelModeCommand(nss) && !isCollectionLevelModeCommand(nss));

            ShardsvrCheckMetadataConsistency shardsvrRequest(nss);
            shardsvrRequest.setDbName(nss.db());
            shardsvrRequest.setCursor(request().getCursor());

            auto catalogCache = Grid::get(opCtx)->catalogCache();
            const auto dbInfo = uassertStatusOK(catalogCache->getDatabase(opCtx, nss.db()));

            auto cmdResponse = executeCommandAgainstDatabasePrimary(
                opCtx,
                nss.db(),
                dbInfo,
                shardsvrRequest.toBSON({}),
                ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                Shard::RetryPolicy::kIdempotent);

            auto response = uassertStatusOK(std::move(cmdResponse.swResponse));
            uassertStatusOK(getStatusFromCommandResult(response.data));

            // TODO: SERVER-72667: Add privileges for getMore()
            auto transformedResponse = uassertStatusOK(
                storePossibleCursor(opCtx,
                                    cmdResponse.shardId,
                                    *cmdResponse.shardHostAndPort,
                                    response.data,
                                    nss,
                                    Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor(),
                                    Grid::get(opCtx)->getCursorManager(),
                                    {}));

            return CheckMetadataConsistencyResponse::parseOwned(
                IDLParserContext("checkMetadataConsistencyResponse"),
                std::move(transformedResponse));
        }

    private:
        NamespaceString ns() const override {
            return request().getNamespace();
        }

        bool supportsWriteConcern() const override {
            return false;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            // TODO: SERVER-72667: Add authorization checks for cluster command
        }
    };
};

MONGO_REGISTER_FEATURE_FLAGGED_COMMAND(CheckMetadataConsistencyCmd,
                                       feature_flags::gCheckMetadataConsistency);

}  // namespace
}  // namespace mongo
