// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/client_cursor/clientcursor.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/util/modules.h"

#include <boost/optional.hpp>

namespace mongo {

class NamespaceString;
class OperationContext;

/**
 * Returns true if we should keep a cursor around because we're expecting to return more query
 * results.
 *
 * If false, the caller should close the cursor and indicate this to the client by sending back
 * a cursor ID of 0.
 */
bool shouldSaveCursor(OperationContext* opCtx, const CollectionPtr& collection, PlanExecutor* exec);

/**
 * Similar to shouldSaveCursor(), but used in getMore to determine whether we should keep the cursor
 * around for additional getMores().
 *
 * If false, the caller should close the cursor and indicate this to the client by sending back a
 * cursor ID of 0.
 */
bool shouldSaveCursorGetMore(PlanExecutor* exec, bool isTailable);

/**
 * 1) Fills out CurOp for "opCtx" with information regarding this query's execution.
 * 2) Reports index usage to the CollectionQueryInfo.
 *
 * Uses explain functionality to extract stats from 'exec'.
 */
void endQueryOp(OperationContext* opCtx,
                const CollectionPtr& collection,
                const PlanExecutor& exec,
                long long numResults,
                boost::optional<ClientCursorPin&> cursor,
                const BSONObj& cmdObj);

}  // namespace mongo
