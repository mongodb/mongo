/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/request_types/auto_split_vector_gen.h"
#include "mongo/s/router_role.h"

#include <memory>
#include <string>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {

namespace {

class ClusterAutoSplitVectorCommand final : public TypedCommand<ClusterAutoSplitVectorCommand> {
public:
    using Request = AutoSplitVectorRequest;

    std::string help() const override {
        return "Returns the split points for a chunk, given a range and the maximum chunk size.";
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        AutoSplitVectorResponse typedRun(OperationContext* opCtx) {
            const auto nss = ns();
            const auto& req = request();

            return routing_context_utils::withValidatedRoutingContext(
                opCtx, {nss}, [&](RoutingContext& routingCtx) {
                    BSONObj filteredCmdObj =
                        CommandHelpers::filterCommandRequestForPassthrough(req.toBSON());

                    // autoSplitVector is allowed to run on a sharded cluster only if the range
                    // requested belongs to one shard. We target the shard owning the input min
                    // chunk and we let the targetted shard figure whether the range is fully owned
                    // by itself. In case the constraint is not respected we will get a
                    // InvalidOptions as part of the response.
                    auto response = scatterGatherVersionedTargetByRoutingTable(
                                        opCtx,
                                        routingCtx,
                                        nss,
                                        filteredCmdObj,
                                        ReadPreferenceSetting::get(opCtx),
                                        Shard::RetryPolicy::kIdempotent,
                                        req.getMin(),
                                        {} /*collation*/,
                                        boost::none /*letParameters*/,
                                        boost::none /*runtimeConstants*/)
                                        .front();

                    auto status = AsyncRequestsSender::Response::getEffectiveStatus(response);
                    uassertStatusOK(status);
                    return AutoSplitVectorResponse::parse(IDLParserContext(""),
                                                          response.swResponse.getValue().data);
                });
        }

    private:
        NamespaceString ns() const override {
            return request().getNamespace();
        }

        bool supportsWriteConcern() const override {
            return false;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(
                            ResourcePattern::forClusterResource(request().getDbName().tenantId()),
                            ActionType::splitVector));
        }
    };
};

MONGO_REGISTER_COMMAND(ClusterAutoSplitVectorCommand).forRouter();

}  // namespace
}  // namespace mongo
