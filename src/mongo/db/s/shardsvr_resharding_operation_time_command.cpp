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

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
// IWYU pragma: no_include "ext/alloc_traits.h"
#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/s/resharding/resharding_metrics.h"
#include "mongo/db/s/resharding/resharding_recipient_service.h"
#include "mongo/db/s/resharding/resharding_server_parameters_gen.h"
#include "mongo/db/s/resharding/resharding_util.h"
#include "mongo/db/service_context.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/s/request_types/resharding_operation_time_gen.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"

#include <memory>
#include <string>
#include <vector>

namespace mongo {
namespace {

class ShardsvrReshardingOperationTimeCmd final
    : public TypedCommand<ShardsvrReshardingOperationTimeCmd> {
public:
    using Request = _shardsvrReshardingOperationTime;
    using Response = ShardsvrReshardingOperationTimeResponse;

    bool skipApiVersionCheck() const override {
        // Internal command (server to server).
        return true;
    }

    std::string help() const override {
        return "Internal command used by the resharding coordinator to query the elapsed and "
               "remaining time for the active resharding operation on participant shards.";
    }

    bool adminOnly() const override {
        return true;
    }

    Command::AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return Command::AllowedOnSecondary::kNever;
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        bool supportsWriteConcern() const override {
            return false;
        }

        NamespaceString ns() const override {
            return request().getCommandParameter();
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(
                            ResourcePattern::forClusterResource(request().getDbName().tenantId()),
                            ActionType::internal));
        }

        Response typedRun(OperationContext* opCtx) {
            ShardingState::get(opCtx)->assertCanAcceptShardedCommands();
            Response response;

            const auto majorityReplicationLag = resharding::getMajorityReplicationLag(opCtx);
            response.setMajorityReplicationLagMillis(majorityReplicationLag);

            auto recipients = resharding::getReshardingStateMachines<
                ReshardingRecipientService,
                ReshardingRecipientService::RecipientStateMachine>(opCtx, ns());

            invariant(recipients.size() <= 1);
            if (!recipients.empty()) {
                const auto& metrics = recipients[0]->getMetrics();

                const auto elapsedTime =
                    duration_cast<Milliseconds>(metrics.getOperationRunningTimeSecs());
                response.setRecipientElapsedMillis(elapsedTime);

                const auto remainingTime = metrics.getHighEstimateRemainingTimeMillis(
                    ReshardingMetrics::CalculationLogOption::Show);
                response.setRecipientRemainingMillis(remainingTime);

                const auto prepareThreshold = Milliseconds(
                    resharding::gRemainingReshardingOperationTimePrepareThresholdMillis.load());
                if (remainingTime <= prepareThreshold) {
                    recipients[0]->prepareForCriticalSection();
                }
            }

            return response;
        }
    };
};
MONGO_REGISTER_COMMAND(ShardsvrReshardingOperationTimeCmd).forShard();

}  // namespace
}  // namespace mongo
