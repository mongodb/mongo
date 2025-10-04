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

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/tailable_mode_gen.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/executor/task_executor.h"
#include "mongo/s/query/exec/owned_remote_cursor.h"
#include "mongo/util/net/hostandport.h"

#include <memory>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

class BSONObj;
class ClusterCursorManager;
class RemoteCursor;
template <typename T>
class StatusWith;
struct HostAndPort;

namespace executor {
class TaskExecutor;
}  // namespace executor

/**
 * Utility function to create a cursor based on existing cursor on a remote instance.  'cmdResult'
 * must be the response object generated upon creation of the cursor. The newly created cursor will
 * use 'executor' to retrieve batches of results from the shards and is stored with 'cursorManager'.
 *
 * 'requestedNss' is used to store the ClusterClientCursor for future lookup. It is also the
 * namespace represented in the cursor response for the returned BSONObj. For views 'requestedNss'
 * may be different then the underlying collection namespace.
 *
 * If 'cmdResult' does not describe a command cursor response document or no cursor is specified,
 * returns 'cmdResult'. If a parsing error occurs, returns an error Status. Otherwise, returns a
 * BSONObj response document describing the newly-created cursor, which is suitable for returning to
 * the client.
 *
 * @ shardId the name of the shard on which the cursor-establishing command was run
 * @ server the exact host in the shard on which the cursor-establishing command was run
 * @ cmdResult the result of running the cursor-establishing command
 * @ requestedNss the namespace on which the client issued the cursor-establishing command (can
 * differ from the execution namespace if the command was issued on a view)
 * @ executor the TaskExecutor to store in the resulting ClusterClientCursor
 * @ cursorManager the ClusterCursorManager on which to register the resulting ClusterClientCursor
 * @ privileges the PrivilegeVector of privileges needed for the original command, to be used for
 * auth checking by GetMore
 * @ routerSort the sort to apply on the router. With only one cursor this shouldn't be common, but
 * is needed to set up change stream post-batch resume tokens correctly for per shard cursors.
 */
StatusWith<BSONObj> storePossibleCursor(OperationContext* opCtx,
                                        const ShardId& shardId,
                                        const HostAndPort& server,
                                        const BSONObj& cmdResult,
                                        const NamespaceString& requestedNss,
                                        std::shared_ptr<executor::TaskExecutor> executor,
                                        ClusterCursorManager* cursorManager,
                                        PrivilegeVector privileges,
                                        TailableModeEnum tailableMode = TailableModeEnum::kNormal,
                                        boost::optional<BSONObj> routerSort = boost::none);

/**
 * Convenience overload taking in a const CursorResponse& instead of a const BSONObj&, as callers
 * who have a CursorResponse instead of a BSONObj would otherwise have to serialize it to call
 * this function, only for this function to parse it again. In addition, parsing a CursorResponse
 * sometimes implies a need to aggregate query stats into the OpDebug. This overload, receiving
 * an already-parsed CursorResponse, never aggregates metrics.
 */
StatusWith<BSONObj> storePossibleCursor(OperationContext* opCtx,
                                        const ShardId& shardId,
                                        const HostAndPort& server,
                                        CursorResponse&& incomingCursorResponse,
                                        const NamespaceString& requestedNss,
                                        std::shared_ptr<executor::TaskExecutor> executor,
                                        ClusterCursorManager* cursorManager,
                                        PrivilegeVector privileges,
                                        TailableModeEnum tailableMode = TailableModeEnum::kNormal,
                                        boost::optional<BSONObj> routerSort = boost::none);

/**
 * Convenience function which extracts all necessary information from the passed RemoteCursor, and
 * stores a ClusterClientCursor based on it. The ownership of the remote cursor is transferred to
 * this function, and will handle killing it upon failure. 'privileges' contains the required
 * privileges for the command, to be used by GetMore for auth checks.
 */
StatusWith<BSONObj> storePossibleCursor(OperationContext* opCtx,
                                        const NamespaceString& requestedNss,
                                        OwnedRemoteCursor&& remoteCursor,
                                        PrivilegeVector privileges,
                                        TailableModeEnum tailableMode);

}  // namespace mongo
