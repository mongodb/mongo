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

#pragma once

#include <boost/optional.hpp>
#include <memory>
#include <set>
#include <utility>
#include <vector>

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/client_cursor/cursor_id.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_id.h"
#include "mongo/executor/task_executor.h"
#include "mongo/platform/mutex.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/query/exec/async_results_merger_params_gen.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/time_support.h"

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
 * @param designatedHostsMap: A map of hosts to be targeted for particular shards, overriding
 *                            the read preference setting.
 *
 */
std::vector<RemoteCursor> establishCursors(
    OperationContext* opCtx,
    std::shared_ptr<executor::TaskExecutor> executor,
    const NamespaceString& nss,
    ReadPreferenceSetting readPref,
    const std::vector<AsyncRequestsSender::Request>& remotes,
    bool allowPartialResults,
    Shard::RetryPolicy retryPolicy = Shard::RetryPolicy::kIdempotent,
    std::vector<OperationKey> providedOpKeys = {},
    AsyncRequestsSender::ShardHostMap designatedHostsMap = {});

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
void appendOpKey(const OperationKey& opKey, BSONObjBuilder* cmdBuilder);

}  // namespace mongo
