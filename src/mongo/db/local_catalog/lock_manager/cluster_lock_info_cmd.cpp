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
#include "mongo/db/dbcommands_gen.h"
#include "mongo/db/global_catalog/router_role_api/cluster_commands_helpers.h"
#include "mongo/db/local_catalog/lock_manager/lock_info_gen.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/database_name_util.h"
#include "mongo/util/decorable.h"

#include <string>
#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo {
namespace {

constexpr auto kRawFieldName = "raw"_sd;
constexpr auto kTopologyVersionFieldName = "topologyVersion"_sd;

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
