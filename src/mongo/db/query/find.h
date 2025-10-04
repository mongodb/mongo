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

#include "mongo/bson/bsonobj.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/client_cursor/clientcursor.h"
#include "mongo/db/query/plan_executor.h"
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
