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
#include "mongo/db/list_indexes_gen.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/query/store_possible_cursor.h"

namespace mongo {
namespace {

ListIndexesReply cursorCommandPassthroughShardWithMinKeyChunk(OperationContext* opCtx,
                                                              const NamespaceString& nss,
                                                              const ChunkManager& cm,
                                                              const BSONObj& cmdObj,
                                                              const PrivilegeVector& privileges) {
    auto response = executeCommandAgainstShardWithMinKeyChunk(
        opCtx,
        nss,
        cm,
        CommandHelpers::filterCommandRequestForPassthrough(cmdObj),
        ReadPreferenceSetting::get(opCtx),
        Shard::RetryPolicy::kIdempotent);
    const auto cmdResponse = uassertStatusOK(std::move(response.swResponse));

    auto transformedResponse = uassertStatusOK(
        storePossibleCursor(opCtx,
                            response.shardId,
                            *response.shardHostAndPort,
                            cmdResponse.data,
                            nss,
                            Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor(),
                            Grid::get(opCtx)->getCursorManager(),
                            privileges));

    BSONObjBuilder out;
    CommandHelpers::filterCommandReplyForPassthrough(transformedResponse, &out);
    const auto& resultObj = out.obj();
    uassertStatusOK(getStatusFromCommandResult(resultObj));
    // The reply syntax must conform to its IDL definition.
    return ListIndexesReply::parse({"listIndexes"}, resultObj);
}

class CmdListIndexes final : public ListIndexesCmdVersion1Gen<CmdListIndexes> {
public:
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        return AllowedOnSecondary::kAlways;
    }
    bool maintenanceOk() const final {
        return false;
    }
    bool adminOnly() const final {
        return false;
    }

    class Invocation final : public InvocationBaseGen {
    public:
        using InvocationBaseGen::InvocationBaseGen;

        bool supportsWriteConcern() const final {
            return false;
        }

        NamespaceString ns() const final {
            const auto& nss = request().getNamespaceOrUUID().nss();
            uassert(
                ErrorCodes::BadValue, "Mongos requires a namespace for listIndexes command", nss);
            return nss.get();
        }

        void doCheckAuthorization(OperationContext* opCtx) const final {
            AuthorizationSession* authzSession = AuthorizationSession::get(opCtx->getClient());
            uassert(ErrorCodes::Unauthorized,
                    str::stream() << "Not authorized to list indexes on collection:"
                                  << ns().toString(),
                    authzSession->isAuthorizedForActionsOnResource(
                        ResourcePattern::forExactNamespace(ns()), ActionType::listIndexes));
        }

        ListIndexesReply typedRun(OperationContext* opCtx) final {
            CommandHelpers::handleMarkKillOnClientDisconnect(opCtx);
            // The command's IDL definition permits namespace or UUID, but mongos requires a
            // namespace.
            const auto cm = uassertStatusOK(
                Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfo(opCtx, ns()));

            return cursorCommandPassthroughShardWithMinKeyChunk(
                opCtx,
                ns(),
                cm,
                applyReadWriteConcern(opCtx, this, request().toBSON({})),
                {Privilege(ResourcePattern::forExactNamespace(ns()), ActionType::listIndexes)});
        }
    };
} cmdListIndexes;

}  // namespace
}  // namespace mongo
