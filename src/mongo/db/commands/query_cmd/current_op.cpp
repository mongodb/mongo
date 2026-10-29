// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
#include "mongo/db/commands/fsync.h"
#include "mongo/db/commands/query_cmd/current_op_common.h"
#include "mongo/db/commands/query_cmd/run_aggregate.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/query/client_cursor/cursor_response.h"
#include "mongo/rpc/op_msg_rpc_impls.h"
#include "mongo/util/serialization_context.h"

#include <utility>

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

    StatusWith<CursorResponse> runAggregation(OperationContext* opCtx,
                                              AggregateCommandRequest& request) const final {
        auto aggCmdObj = request.toBSON();

        rpc::OpMsgReplyBuilder replyBuilder;

        PrivilegeVector privileges;
        if (!aggCmdObj["$ownOps"].trueValue()) {
            privileges = {
                Privilege(ResourcePattern::forClusterResource(request.getDbName().tenantId()),
                          ActionType::inprog)};
        }

        auto status = runAggregate(opCtx,
                                   request,
                                   {request},
                                   std::move(aggCmdObj),
                                   privileges,
                                   boost::none, /* verbosity */
                                   &replyBuilder);

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

    void appendToResponse(BSONObjBuilder* result) const final {
        if (lockedForWriting()) {
            result->append("fsyncLock", true);
            result->append("info",
                           "use db.fsyncUnlock() to terminate the fsync write/snapshot lock");
        }
    }
};
MONGO_REGISTER_COMMAND(CurrentOpCommand).forShard();

}  // namespace mongo
