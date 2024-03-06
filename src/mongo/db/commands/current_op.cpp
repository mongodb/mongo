/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include <utility>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/current_op_common.h"
#include "mongo/db/commands/fsync_locked.h"
#include "mongo/db/commands/run_aggregate.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/aggregation_request_helper.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/rpc/op_msg_rpc_impls.h"
#include "mongo/util/serialization_context.h"

namespace mongo {

class CurrentOpCommand final : public CurrentOpCommandBase {
    CurrentOpCommand(const CurrentOpCommand&) = delete;
    CurrentOpCommand& operator=(const CurrentOpCommand&) = delete;

public:
    CurrentOpCommand() = default;

    Status checkAuthForOperation(OperationContext* opCtx,
                                 const DatabaseName& dbName,
                                 const BSONObj& cmdObj) const final {
        AuthorizationSession* authzSession = AuthorizationSession::get(opCtx->getClient());
        if (authzSession->isAuthorizedForActionsOnResource(
                ResourcePattern::forClusterResource(dbName.tenantId()), ActionType::inprog)) {
            return Status::OK();
        }

        if (authzSession->isAuthenticated() && cmdObj["$ownOps"].trueValue()) {
            return Status::OK();
        }

        return Status(ErrorCodes::Unauthorized, "Unauthorized");
    }

    bool allowedWithSecurityToken() const final {
        return true;
    }

    virtual StatusWith<CursorResponse> runAggregation(
        OperationContext* opCtx, AggregateCommandRequest& request) const final {
        auto aggCmdObj = aggregation_request_helper::serializeToCommandObj(request);

        rpc::OpMsgReplyBuilder replyBuilder;

        PrivilegeVector privileges;
        if (!aggCmdObj["$ownOps"].trueValue()) {
            privileges = {
                Privilege(ResourcePattern::forClusterResource(request.getDbName().tenantId()),
                          ActionType::inprog)};
        }

        auto status = runAggregate(
            opCtx, request, {request}, std::move(aggCmdObj), privileges, &replyBuilder);

        if (!status.isOK()) {
            return status;
        }

        auto bodyBuilder = replyBuilder.getBodyBuilder();
        CommandHelpers::appendSimpleCommandStatus(bodyBuilder, true);
        bodyBuilder.doneFast();

        // We need to copy the serialization context from the request to the reply object
        return CursorResponse::parseFromBSON(
            replyBuilder.releaseBody(),
            nullptr,
            request.getNamespace().tenantId(),
            SerializationContext::stateCommandReply(request.getSerializationContext()));
    }

    virtual void appendToResponse(BSONObjBuilder* result) const final {
        if (lockedForWriting()) {
            result->append("fsyncLock", true);
            result->append("info",
                           "use db.fsyncUnlock() to terminate the fsync write/snapshot lock");
        }
    }
};
MONGO_REGISTER_COMMAND(CurrentOpCommand).forShard();

}  // namespace mongo
