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


#include "mongo/platform/basic.h"

#include <iostream>
#include <string>

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/logv2/log.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/grid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
namespace {

/**
 * Resets the commitQuorum set on an index build identified by the list of index names that were
 * previously specified in a createIndexes request.
 *
 * {
 *     setIndexCommitQuorum: coll,
 *     indexNames: ["x_1", "y_1", "xIndex", "someindexname"],
 *     commitQuorum: "majority" / 3 / {"replTagName": "replTagValue"},
 * }
 */
class SetIndexCommitQuorumCommand : public BasicCommand {
public:
    SetIndexCommitQuorumCommand() : BasicCommand("setIndexCommitQuorum") {}

    std::string help() const override {
        std::stringstream ss;
        ss << "Resets the commitQuorum for the given index builds in a collection. Usage:"
           << std::endl
           << "{" << std::endl
           << "    setIndexCommitQuorum: <string> collection name," << std::endl
           << "    indexNames: array<string> list of index names," << std::endl
           << "    commitQuorum: <string|number|object> option to define the required quorum for"
           << std::endl
           << "                  the index builds to commit" << std::endl
           << "}";
        return ss.str();
    }

    bool adminOnly() const override {
        return false;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const final {
        return true;
    }

    Status checkAuthForOperation(OperationContext* opCtx,
                                 const std::string& dbName,
                                 const BSONObj& cmdObj) const override {
        const NamespaceString nss(
            CommandHelpers::parseNsCollectionRequired({boost::none, dbName}, cmdObj));
        if (!AuthorizationSession::get(opCtx->getClient())
                 ->isAuthorizedForActionsOnResource(ResourcePattern::forExactNamespace(nss),
                                                    ActionType::createIndex)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }
        return Status::OK();
    }

    bool run(OperationContext* opCtx,
             const DatabaseName& dbName,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        const NamespaceString nss(CommandHelpers::parseNsCollectionRequired(dbName, cmdObj));
        LOGV2_DEBUG(22757,
                    1,
                    "setIndexCommitQuorum",
                    "namespace"_attr = nss,
                    "command"_attr = redact(cmdObj));

        auto routingInfo =
            uassertStatusOK(Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfo(opCtx, nss));
        auto shardResponses = scatterGatherVersionedTargetByRoutingTable(
            opCtx,
            nss.db(),
            nss,
            routingInfo,
            applyReadWriteConcern(
                opCtx, this, CommandHelpers::filterCommandRequestForPassthrough(cmdObj)),
            ReadPreferenceSetting::get(opCtx),
            Shard::RetryPolicy::kNotIdempotent,
            BSONObj() /* query */,
            BSONObj() /* collation */);

        std::string errmsg;
        const bool ok =
            appendRawResponses(opCtx, &errmsg, &result, std::move(shardResponses)).responseOK;
        CommandHelpers::appendSimpleCommandStatus(result, ok, errmsg);

        if (ok) {
            LOGV2(5688700, "Index commit quorums set", "namespace"_attr = nss);
        }

        return ok;
    }

} setCommitQuorumCmd;

}  // namespace
}  // namespace mongo
