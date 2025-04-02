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


#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/dbcommands_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/legacy_runtime_constants_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/grid.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
namespace {

class DataSizeCmd : public TypedCommand<DataSizeCmd> {
public:
    using Request = DataSizeCommand;
    using Reply = typename Request::Reply;

    DataSizeCmd() : TypedCommand(Request::kCommandName, Request::kCommandAlias) {}

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        NamespaceString ns() const final {
            return request().getCommandParameter();
        }

        bool supportsWriteConcern() const final {
            return false;
        }

        void doCheckAuthorization(OperationContext* opCtx) const final {
            auto* as = AuthorizationSession::get(opCtx->getClient());
            uassert(ErrorCodes::Unauthorized,
                    "unauthorized",
                    as->isAuthorizedForActionsOnResource(ResourcePattern::forExactNamespace(ns()),
                                                         ActionType::find));
        }

        Reply typedRun(OperationContext* opCtx) {
            auto& cmd = request();
            const auto& nss = ns();

            auto cri = uassertStatusOK(
                Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfo(opCtx, nss));

            setReadWriteConcern(opCtx, cmd, this);
            auto shardResults = scatterGatherVersionedTargetByRoutingTable(
                opCtx,
                nss.dbName(),
                nss,
                cri,
                CommandHelpers::filterCommandRequestForPassthrough(cmd.toBSON()),
                ReadPreferenceSetting::get(opCtx),
                Shard::RetryPolicy::kIdempotent,
                {} /*query*/,
                {} /*collation*/,
                boost::none /*letParameters*/,
                boost::none /*runtimeConstants*/);

            std::int64_t size = 0;
            std::int64_t numObjects = 0;
            std::int64_t millis = 0;

            for (const auto& shardResult : shardResults) {
                const auto shardResponse = uassertStatusOK(std::move(shardResult.swResponse));
                uassertStatusOK(shardResponse.status);

                const auto& res = shardResponse.data;
                uassertStatusOK(getStatusFromCommandResult(res));

                auto parsedResponse = Reply::parse(IDLParserContext{"dataSize"}, res);
                size += parsedResponse.getSize();
                numObjects += parsedResponse.getNumObjects();
                millis += parsedResponse.getMillis();
            }

            Reply reply;
            reply.setSize(size);
            reply.setNumObjects(numObjects);
            reply.setMillis(millis);
            return reply;
        }
    };

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        return AllowedOnSecondary::kAlways;
    }

    bool adminOnly() const final {
        return false;
    }
};
MONGO_REGISTER_COMMAND(DataSizeCmd).forRouter();

}  // namespace
}  // namespace mongo
