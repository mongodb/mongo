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

#include <string>

#include "mongo/db/clientcursor.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/rpc/message.h"

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
bool shouldSaveCursor(OperationContext* opCtx,
                      const Collection* collection,
                      PlanExecutor::ExecState finalState,
                      PlanExecutor* exec);

/**
 * Similar to shouldSaveCursor(), but used in getMore to determine whether we should keep
 * the cursor around for additional getMores().
 *
 * If false, the caller should close the cursor and indicate this to the client by sending back
 * a cursor ID of 0.
 */
bool shouldSaveCursorGetMore(PlanExecutor::ExecState finalState,
                             PlanExecutor* exec,
                             bool isTailable);

/**
 * Fills out the CurOp for "opCtx" with information about this query.
 */
void beginQueryOp(OperationContext* opCtx,
                  const NamespaceString& nss,
                  const BSONObj& queryObj,
                  long long ntoreturn,
                  long long ntoskip);

/**
 * 1) Fills out CurOp for "opCtx" with information regarding this query's execution.
 * 2) Reports index usage to the CollectionInfoCache.
 *
 * Uses explain functionality to extract stats from 'exec'.
 */
void endQueryOp(OperationContext* opCtx,
                Collection* collection,
                const PlanExecutor& exec,
                long long numResults,
                CursorId cursorId);

/**
 * Called from the getMore entry point in ops/query.cpp.
 * Returned buffer is the message to return to the client.
 */
Message getMore(OperationContext* opCtx,
                const char* ns,
                int ntoreturn,
                long long cursorid,
                bool* exhaust,
                bool* isCursorAuthorized);

/**
 * Run the query 'q' and place the result in 'result'.
 */
std::string runQuery(OperationContext* opCtx,
                     QueryMessage& q,
                     const NamespaceString& ns,
                     Message& result);

}  // namespace mongo
