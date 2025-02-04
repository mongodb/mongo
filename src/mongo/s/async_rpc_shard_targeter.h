/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#pragma once

#include <boost/move/utility_core.hpp>
#include <boost/smart_ptr.hpp>
#include <memory>
#include <tuple>
#include <utility>
#include <vector>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/client/read_preference.h"
#include "mongo/client/remote_command_targeter.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_id.h"
#include "mongo/executor/async_rpc_targeter.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/future.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/out_of_line_executor.h"


namespace mongo {
namespace async_rpc {

class ShardIdTargeter : public Targeter {
public:
    ShardIdTargeter(ExecutorPtr executor,
                    OperationContext* opCtx,
                    ShardId shardId,
                    ReadPreferenceSetting readPref)
        : _executor(executor), _opCtx(opCtx), _shardId(shardId), _readPref(readPref){};

    SemiFuture<std::vector<HostAndPort>> resolve(CancellationToken t) override {
        return getShard()
            .thenRunOn(_executor)
            .then([this, t](std::shared_ptr<Shard> shard) {
                _shardFromLastResolve = shard;
                return shard->getTargeter()->findHosts(_readPref, t);
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
