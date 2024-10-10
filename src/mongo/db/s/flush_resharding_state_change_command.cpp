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
#include <boost/smart_ptr.hpp>
#include <memory>
#include <string>
#include <tuple>
#include <utility>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/s/resharding/resharding_util.h"
#include "mongo/db/s/shard_filtering_metadata_refresh.h"
#include "mongo/db/service_context.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/redaction.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/flush_resharding_state_change_gen.h"
#include "mongo/s/sharding_state.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/future.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/uuid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kResharding


namespace mongo {
namespace {
class FlushReshardingStateChangeCmd final : public TypedCommand<FlushReshardingStateChangeCmd> {
public:
    using Request = _flushReshardingStateChange;

    bool skipApiVersionCheck() const override {
        // Internal command (server to server).
        return true;
    }

    std::string help() const override {
        return "Internal command used by the resharding coordinator to flush state changes to the "
               "participant shards while the critical section is active.";
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
            return true;
        }

        NamespaceString ns() const override {
            return request().getCommandParameter();
        }

        UUID reshardingUUID() const {
            return request().getReshardingUUID();
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(
                            ResourcePattern::forClusterResource(request().getDbName().tenantId()),
                            ActionType::internal));
        }

        void typedRun(OperationContext* opCtx) {
            auto const shardingState = ShardingState::get(opCtx);
            shardingState->assertCanAcceptShardedCommands();

            uassert(ErrorCodes::IllegalOperation,
                    "Can't issue _flushReshardingStateChange from 'eval'",
                    !opCtx->getClient()->isInDirectClient());

            uassert(ErrorCodes::IllegalOperation,
                    "Can't call _flushReshardingStateChange if in read-only mode",
                    !opCtx->readOnly());

            // We use the fixed executor here since it may cause the thread to block. This would
            // cause potential liveness issues since the arbitrary executor is a NetworkInterfaceTL
            // executor in sharded clusters and that executor is one that executes networking
            // operations.
            ExecutorFuture<void>(Grid::get(opCtx)->getExecutorPool()->getFixedExecutor())
                .then([svcCtx = opCtx->getServiceContext(), nss = ns()] {
                    ThreadClient tc("FlushReshardingStateChange",
                                    svcCtx->getService(ClusterRole::ShardServer));
                    auto opCtx = tc->makeOperationContext();
                    FilteringMetadataCache::get(opCtx.get())
                        ->onCollectionPlacementVersionMismatch(
                            opCtx.get(), nss, boost::none /* chunkVersionReceived */);
                })
                .onError([](const Status& status) {
                    LOGV2_WARNING(5808100,
                                  "Error on deferred _flushReshardingStateChange execution",
                                  "error"_attr = redact(status));
                })
                .getAsync([](auto) {});

            // Ensure the command isn't run on a stale primary.
            resharding::doNoopWrite(opCtx, "_flushReshardingStateChange no-op", ns());
        }
    };
};
MONGO_REGISTER_COMMAND(FlushReshardingStateChangeCmd).forShard();

}  // namespace
}  // namespace mongo
