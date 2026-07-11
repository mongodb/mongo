// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/router_role/router_role.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/executor/task_executor.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/s/query/exec/async_results_merger_params_gen.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/modules.h"

#include <memory>
#include <set>
#include <vector>

#include <boost/optional.hpp>

namespace mongo {

/**
 * Establishes cursors on the remote shards by issuing requests in parallel, using the readPref to
 * select a host within each shard.
 *
 * If any of the cursors fail to be established, this function performs cleanup by sending
 * killCursors to any cursors that were established, then throws the error. If the namespace
 * represents a view, an exception containing a ResolvedView is thrown. Calling code can then
 * attempt to establish cursors against the base collection using this view.
 *
 * On success, the ownership of the cursors is transferred to the caller. This means the caller is
 * now responsible for either exhausting the cursors or sending killCursors to them.
 *
 * If providedOpKeys are given, this assumes all requests have been given an operation key and will
 * use the provided keys to kill operations on failure. Otherwise a unique operation key is
 * generated and attached to all requests.
 *
 * @param allowPartialResults: If true, unreachable hosts are ignored, and only cursors established
 *                             on reachable hosts are returned.
 *
 * @param routingCtx: An interface that acquires cached routing tables at the start of a routing
 *                    operation and provides accessors throughout the operation. If provided, we
 *                    will mark that a request has successfully sent a request to the shards at the
 *                    end of CursorEstablisher::sendRequests(), in order to validate the
 *                    RoutingContext.
 *
 * @param designatedHostsMap: A map of hosts to be targeted for particular shards, overriding
 *                            the read preference setting.
 *
 * TODO SERVER-111290 Remove external dependencies on this method.
 */
[[MONGO_MOD_NEEDS_REPLACEMENT]] std::vector<RemoteCursor> establishCursors(
    OperationContext* opCtx,
    std::shared_ptr<executor::TaskExecutor> executor,
    const NamespaceString& nss,
    ReadPreferenceSetting readPref,
    const std::vector<AsyncRequestsSender::Request>& remotes,
    bool allowPartialResults,
    RoutingContext* routingCtx = nullptr,
    Shard::RetryPolicy retryPolicy = Shard::RetryPolicy::kIdempotent,
    std::vector<OperationKey> providedOpKeys = {},
    const AsyncRequestsSender::ShardHostMap& designatedHostsMap = {});

/**
 * Establishes cursors on every host in the remote shards by issuing requests in parallel with the
 * AsyncMulticaster.
 *
 * If any of the cursors fail to be established, this function performs cleanup by sending
 * killCursors to any cursors that were established, then throws the error. If the namespace
 * represents a view, an exception containing a ResolvedView is thrown.
 *
 * On success, the ownership of the cursors is transferred to the caller. This means the caller is
 * now responsible for either exhausting the cursors or sending killCursors to them.
 */
std::vector<RemoteCursor> establishCursorsOnAllHosts(
    OperationContext* opCtx,
    RoutingContext& routingCtx,
    std::shared_ptr<executor::TaskExecutor> executor,
    const NamespaceString& nss,
    const std::set<ShardId>& shardIds,
    BSONObj cmdObj,
    bool allowPartialResults,
    Shard::RetryPolicy retryPolicy = Shard::RetryPolicy::kIdempotent);

/**
 * Schedules a remote killCursor command for 'cursor'.
 *
 * Note that this method is optimistic and does not check the return status for the killCursors
 * command.
 */
void killRemoteCursor(OperationContext* opCtx,
                      executor::TaskExecutor* executor,
                      RemoteCursor&& cursor,
                      const NamespaceString& nss);

/**
 * Appends the given operation key to the given request.
 */
[[MONGO_MOD_NEEDS_REPLACEMENT]] void appendOpKey(const OperationKey& opKey,
                                                 BSONObjBuilder* cmdBuilder);

/**
 * Resets the log severity suppressor to use the given clock source (pass nullptr to restore the
 * real system clock). This allows tests to advance time deterministically instead of relying on
 * real sleeps. Not thread-safe, so use only during controlled testing.
 */
void setCursorEstablisherSuppressorClockSource_forTest(ClockSource* cs);

}  // namespace mongo
