/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/auth/authorization_session.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/grid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
namespace {

bool nonShardedCollectionCommandPassthrough(OperationContext* opCtx,
                                            const DatabaseName& dbName,
                                            const NamespaceString& nss,
                                            const CollectionRoutingInfo& cri,
                                            const BSONObj& cmdObj,
                                            Shard::RetryPolicy retryPolicy,
                                            BSONObjBuilder* out) {
    const StringData cmdName(cmdObj.firstElementFieldName());
    uassert(ErrorCodes::IllegalOperation,
            str::stream() << "Can't do command: " << cmdName << " on a sharded collection",
            !cri.cm.isSharded());

    auto responses = scatterGatherVersionedTargetByRoutingTable(opCtx,
                                                                dbName.toStringWithTenantId(),
                                                                nss,
                                                                cri,
                                                                cmdObj,
                                                                ReadPreferenceSetting::get(opCtx),
                                                                retryPolicy,
                                                                {} /*query*/,
                                                                {} /*collation*/,
                                                                boost::none /*letParameters*/,
                                                                boost::none /*runtimeConstants*/);
    invariant(responses.size() == 1);

    const auto cmdResponse = uassertStatusOK(std::move(responses.front().swResponse));
    const auto status = getStatusFromCommandResult(cmdResponse.data);

    uassert(ErrorCodes::IllegalOperation,
            str::stream() << "Can't do command: " << cmdName << " on a sharded collection",
            !ErrorCodes::isStaleShardVersionError(status));

    out->appendElementsUnique(CommandHelpers::filterCommandReplyForPassthrough(cmdResponse.data));
    return status.isOK();
}

class ConvertToCappedCmd : public BasicCommand {
public:
    ConvertToCappedCmd() : BasicCommand("convertToCapped") {}

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    NamespaceString parseNs(const DatabaseName& dbName, const BSONObj& cmdObj) const override {
        return CommandHelpers::parseNsCollectionRequired(dbName, cmdObj);
    }

    Status checkAuthForOperation(OperationContext* opCtx,
                                 const DatabaseName& dbName,
                                 const BSONObj& cmdObj) const override {
        auto* as = AuthorizationSession::get(opCtx->getClient());
        if (!as->isAuthorizedForActionsOnResource(parseResourcePattern(dbName, cmdObj),
                                                  ActionType::convertToCapped)) {
            return {ErrorCodes::Unauthorized, "unauthorized"};
        }

        return Status::OK();
    }

    bool run(OperationContext* opCtx,
             const DatabaseName& dbName,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        const NamespaceString nss(parseNs(dbName, cmdObj));
        const auto cri =
            uassertStatusOK(Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfo(opCtx, nss));
        uassert(ErrorCodes::IllegalOperation,
                "You can't convertToCapped a sharded collection",
                !cri.cm.isSharded());

        // convertToCapped creates a temp collection and renames it at the end. It will require
        // special handling for create collection.
        return nonShardedCollectionCommandPassthrough(
            opCtx,
            dbName,
            nss,
            cri,
            applyReadWriteConcern(
                opCtx, this, CommandHelpers::filterCommandRequestForPassthrough(cmdObj)),
            Shard::RetryPolicy::kIdempotent,
            &result);
    }

} convertToCappedCmd;

}  // namespace
}  // namespace mongo
