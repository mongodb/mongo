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
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/query/store_possible_cursor.h"

namespace mongo {
namespace {

bool cursorCommandPassthroughShardWithMinKeyChunk(OperationContext* opCtx,
                                                  const NamespaceString& nss,
                                                  const ChunkManager& cm,
                                                  const BSONObj& cmdObj,
                                                  BSONObjBuilder* out,
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

    CommandHelpers::filterCommandReplyForPassthrough(transformedResponse, out);
    // The reply syntax must conform to its IDL definition.
    ListIndexesReply::parse({"listIndexes"}, out->asTempObj());
    return true;
}

class CmdListIndexes : public BasicCommand {
public:
    CmdListIndexes() : BasicCommand("listIndexes") {}

    const std::set<std::string>& apiVersions() const {
        return kApiVersions1;
    }

    std::string parseNs(const std::string& dbname, const BSONObj& cmdObj) const override {
        return CommandHelpers::parseNsCollectionRequired(dbname, cmdObj).ns();
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    bool maintenanceOk() const override {
        return false;
    }

    bool adminOnly() const override {
        return false;
    }

    Status checkAuthForCommand(Client* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) const override {
        AuthorizationSession* authzSession = AuthorizationSession::get(client);

        // Check for the listIndexes ActionType on the database.
        const NamespaceString ns(parseNs(dbname, cmdObj));

        if (authzSession->isAuthorizedForActionsOnResource(ResourcePattern::forExactNamespace(ns),
                                                           ActionType::listIndexes)) {
            return Status::OK();
        }

        return Status(ErrorCodes::Unauthorized,
                      str::stream()
                          << "Not authorized to list indexes on collection: " << ns.coll());
    }

    bool run(OperationContext* opCtx,
             const std::string& dbName,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        CommandHelpers::handleMarkKillOnClientDisconnect(opCtx);
        // Check the command syntax before passing to shard.
        const auto parsed = ListIndexes::parse({"listIndexes"}, cmdObj);

        // The command's IDL definition permits namespace or UUID, but mongos requires a namespace.
        const NamespaceString nss(parseNs(dbName, cmdObj));
        const auto cm =
            uassertStatusOK(Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfo(opCtx, nss));

        return cursorCommandPassthroughShardWithMinKeyChunk(
            opCtx,
            nss,
            cm,
            applyReadWriteConcern(opCtx, this, cmdObj),
            &result,
            {Privilege(ResourcePattern::forExactNamespace(nss), ActionType::listIndexes)});
    }

} cmdListIndexes;

}  // namespace
}  // namespace mongo
