// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
#include "mongo/util/modules.h"
#include "mongo/util/net/hostandport.h"

#include <functional>
#include <memory>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace [[MONGO_MOD_PUBLIC]] mongo {

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
 * @ allowPartialResults whether future getMore requests should tolerate unreachable remotes
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
                                        boost::optional<BSONObj> routerSort = boost::none,
                                        bool allowPartialResults = false);

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
                                        boost::optional<BSONObj> routerSort = boost::none,
                                        bool allowPartialResults = false);

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
                                        TailableModeEnum tailableMode,
                                        bool allowPartialResults = false);

/**
 * Overload of storePossibleCursor(), but applies 'documentTransform' to every result document, both
 * in the initial first batch and in all subsequent getMore batches. The transform is injected into
 * the cursor's execution plan via RouterStageTransform.
 *
 * Use this when a command-level response transformation must be applied uniformly across all
 * cursor batches (e.g., stripping internal fields before sending response to clients).
 */
StatusWith<BSONObj> storePossibleCursor(OperationContext* opCtx,
                                        const ShardId& shardId,
                                        const HostAndPort& server,
                                        const BSONObj& cmdResult,
                                        const NamespaceString& requestedNss,
                                        std::shared_ptr<executor::TaskExecutor> executor,
                                        ClusterCursorManager* cursorManager,
                                        PrivilegeVector privileges,
                                        std::function<BSONObj(BSONObj)> documentTransform,
                                        TailableModeEnum tailableMode = TailableModeEnum::kNormal,
                                        boost::optional<BSONObj> routerSort = boost::none,
                                        bool allowPartialResults = false);

}  // namespace mongo
