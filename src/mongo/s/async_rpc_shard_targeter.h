// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/client/read_preference.h"
#include "mongo/client/remote_command_targeter.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/shard_catalog/operation_sharding_state.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/executor/async_rpc_targeter.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/future.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/modules.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/out_of_line_executor.h"

#include <memory>
#include <tuple>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/smart_ptr.hpp>


namespace mongo {
namespace async_rpc {

class [[MONGO_MOD_OPEN]] ShardIdTargeter : public Targeter {
public:
    ShardIdTargeter(ExecutorPtr executor,
                    OperationContext* opCtx,
                    ShardId shardId,
                    ReadPreferenceSetting readPref)
        : _executor(executor), _opCtx(opCtx), _shardId(shardId), _readPref(readPref) {};

    SemiFuture<HostAndPort> resolve(CancellationToken t,
                                    const TargetingMetadata& targetingMetadata) override {
        return getShard()
            .thenRunOn(_executor)
            .then([this, t, targetingMetadata](std::shared_ptr<Shard> shard) {
                _shardFromLastResolve = shard;
                return shard->getTargeter()->findHost(_readPref, t, targetingMetadata);
            })
            .semi();
    }

    /**
     * Update underlying shard targeter's view of topology on error.
     */
    SemiFuture<void> onRemoteCommandError(HostAndPort h, Status s) override {
        invariant(_shardFromLastResolve,
                  "Cannot propagate a remote command error to a ShardTargeter before calling "
                  "resolve and obtaining a shard.");
        _shardFromLastResolve->updateReplSetMonitor(h, s);
        return SemiFuture<void>::makeReady();
    }

    SemiFuture<std::shared_ptr<Shard>> getShard() {
        return Grid::get(_opCtx)->shardRegistry()->getShard(_executor, _shardId);
    }

    ShardId getShardId() {
        return _shardId;
    }

private:
    ExecutorPtr _executor;
    OperationContext* _opCtx;
    ShardId _shardId;
    ReadPreferenceSetting _readPref;
    std::shared_ptr<Shard> _shardFromLastResolve;
};

}  // namespace async_rpc
}  // namespace mongo
