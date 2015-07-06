/**
 *    Copyright (C) 2013 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
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
#include "mongo/util/net/message.h"

namespace mongo {

class NamespaceString;
class OperationContext;

class ScopedRecoveryUnitSwapper {
public:
    ScopedRecoveryUnitSwapper(ClientCursor* cc, OperationContext* txn);

    ~ScopedRecoveryUnitSwapper();

    /**
     * Dismissing the RU swapper causes it to simply free the recovery unit rather than swapping
     * it back into the ClientCursor.
     */
    void dismiss();

private:
    ClientCursor* _cc;
    OperationContext* _txn;
    bool _dismissed;

    std::unique_ptr<RecoveryUnit> _txnPreviousRecoveryUnit;
    OperationContext::RecoveryUnitState _txnPreviousRecoveryUnitState;
};

/**
 * Returns true if enough results have been prepared to stop adding more to the first batch.
 *
 * Should be called *after* adding to the result set rather than before.
 */
bool enoughForFirstBatch(const LiteParsedQuery& pq, long long numDocs, int bytesBuffered);

/**
 * Returns true if enough results have been prepared to stop adding more to a getMore batch.
 *
 * Should be called *after* adding to the result set rather than before.
 */
bool enoughForGetMore(long long ntoreturn, long long numDocs, int bytesBuffered);

/**
 * Whether or not the ClientCursor* is tailable.
 */
bool isCursorTailable(const ClientCursor* cursor);

/**
 * Whether or not the ClientCursor* has the awaitData flag set.
 */
bool isCursorAwaitData(const ClientCursor* cursor);

/**
 * Returns true if we should keep a cursor around because we're expecting to return more query
 * results.
 *
 * If false, the caller should close the cursor and indicate this to the client by sending back
 * a cursor ID of 0.
 */
bool shouldSaveCursor(OperationContext* txn,
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
 * Fills out the CurOp for "txn" with information about this query.
 */
void beginQueryOp(OperationContext* txn,
                  const NamespaceString& nss,
                  const BSONObj& queryObj,
                  long long ntoreturn,
                  long long ntoskip);

/**
 * Fills out CurOp for "txn" with information regarding this query's execution.
 *
 * Uses explain functionality to extract stats from 'exec'.
 *
 * The database profiling level, 'dbProfilingLevel', is used to conditionalize whether or not we
 * do expensive stats gathering.
 */
void endQueryOp(OperationContext* txn,
                const PlanExecutor& exec,
                int dbProfilingLevel,
                long long numResults,
                CursorId cursorId);

/**
 * Constructs a PlanExecutor for a query with the oplogReplay option set to true,
 * for the query 'cq' over the collection 'collection'. The PlanExecutor will
 * wrap a singleton OplogStart stage.
 *
 * The oplog start finding hack requires that 'cq' has a $gt or $gte predicate over
 * a field named 'ts'.
 */
StatusWith<std::unique_ptr<PlanExecutor>> getOplogStartHack(OperationContext* txn,
                                                            Collection* collection,
                                                            std::unique_ptr<CanonicalQuery> cq);

/**
 * Called from the getMore entry point in ops/query.cpp.
 */
QueryResult::View getMore(OperationContext* txn,
                          const char* ns,
                          int ntoreturn,
                          long long cursorid,
                          int pass,
                          bool& exhaust,
                          bool* isCursorAuthorized);

/**
 * Run the query 'q' and place the result in 'result'.
 */
std::string runQuery(OperationContext* txn,
                     QueryMessage& q,
                     const NamespaceString& ns,
                     Message& result);

}  // namespace mongo
