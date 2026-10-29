// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/database_name_util.h"
#include "mongo/db/dbcommands_gen.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/router_role/cluster_commands_helpers.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/lock_manager/lock_info_gen.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"

#include <string>
#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

constexpr auto kRawFieldName = "raw"sv;
constexpr auto kTopologyVersionFieldName = "topologyVersion"sv;

class ClusterLockInfoCmd final : public BasicCommandWithRequestParser<ClusterLockInfoCmd> {
public:
    using Request = LockInfoCommand;
    using Reply = LockInfoReply;

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        return AllowedOnSecondary::kAlways;
    }

    bool maintenanceOk() const final {
        return false;
    }

    bool adminOnly() const final {
        return true;
    }

    bool supportsWriteConcern(const BSONObj&) const final {
        return false;
    }

    Status checkAuthForOperation(OperationContext* opCtx,
                                 const DatabaseName& dbName,
                                 const BSONObj&) const override {
        auto* as = AuthorizationSession::get(opCtx->getClient());
        if (!as->isAuthorizedForActionsOnResource(
                ResourcePattern::forClusterResource(dbName.tenantId()), ActionType::serverStatus)) {
            return {ErrorCodes::Unauthorized, "unauthorized"};
        }
        return Status::OK();
    }

    bool runWithRequestParser(OperationContext* opCtx,
                              const DatabaseName& dbName,
                              const BSONObj& cmdObj,
                              const RequestParser& requestParser,
                              BSONObjBuilder& output) final {
        std::string errmsg;
        auto shardResponses = scatterGatherUnversionedTargetAllShards(
            opCtx,
            dbName,
            applyReadWriteConcern(
                opCtx, this, CommandHelpers::filterCommandRequestForPassthrough(cmdObj)),
            ReadPreferenceSetting::get(opCtx),
            Shard::RetryPolicy::kIdempotent);

        const bool ok = appendRawResponses(opCtx, &errmsg, &output, shardResponses).responseOK;

        // This command has global scope. As such, the logic of promoting the response of a shard
        // when the command is run on an unsharded collection is not applicable. For the sake of
        // interface parity between replica sets and 1-shard clusters, if there is a single
        // response, we assume this is a 1-shard cluster and the raw response is appended to the
        // top level.
        if (shardResponses.size() == 1 && ok) {
            CommandHelpers::filterCommandReplyForPassthrough(
                shardResponses[0].swResponse.getValue().data, &output);
        }

        CommandHelpers::appendSimpleCommandStatus(output, ok, errmsg);
        return ok;
    }


    void validateResult(const BSONObj& result) final {
        auto ctx = IDLParserContext("LockInfoReply");
        if (checkIsErrorStatus(result, ctx)) {
            return;
        }

        StringDataSet ignorableFields({kTopologyVersionFieldName, kRawFieldName});
        Reply::parse(result.removeFields(ignorableFields), ctx);
        if (!result.hasField(kRawFieldName)) {
            return;
        }

        const auto& rawData = result[kRawFieldName];
        if (!ctx.checkAndAssertType(rawData, BSONType::object)) {
            return;
        }

        auto rawCtx = IDLParserContext(kRawFieldName, &ctx);
        for (const auto& element : rawData.Obj()) {
            if (!rawCtx.checkAndAssertType(element, BSONType::object)) {
                return;
            }

            const auto& shardReply = element.Obj();
            if (!checkIsErrorStatus(shardReply, ctx)) {
                Reply::parse(shardReply.removeFields(ignorableFields), ctx);
            }
        }
    }
};
MONGO_REGISTER_COMMAND(ClusterLockInfoCmd).forRouter();

}  // namespace
}  // namespace mongo
