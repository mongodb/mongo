/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/query/internal_plans.h"

#include "mongo/db/catalog/database.h"
#include "mongo/db/client.h"
#include "mongo/db/exec/collection_scan.h"
#include "mongo/db/exec/eof.h"
#include "mongo/db/exec/fetch.h"
#include "mongo/db/exec/index_scan.h"
#include "mongo/db/query/plan_executor.h"

namespace mongo {

    // static
    PlanExecutor* InternalPlanner::collectionScan(OperationContext* txn,
                                                  StringData ns,
                                                  Collection* collection,
                                                  const Direction direction,
                                                  const RecordId startLoc) {
        WorkingSet* ws = new WorkingSet();

        if (NULL == collection) {
            EOFStage* eof = new EOFStage();
            PlanExecutor* exec;
            // Takes ownership of 'ws' and 'eof'.
            Status execStatus =  PlanExecutor::make(txn,
                                                    ws,
                                                    eof,
                                                    ns.toString(),
                                                    PlanExecutor::YIELD_MANUAL,
                                                    &exec);
            invariant(execStatus.isOK());
            return exec;
        }

        invariant( ns == collection->ns().ns() );

        CollectionScanParams params;
        params.collection = collection;
        params.start = startLoc;

        if (FORWARD == direction) {
            params.direction = CollectionScanParams::FORWARD;
        }
        else {
            params.direction = CollectionScanParams::BACKWARD;
        }

        CollectionScan* cs = new CollectionScan(txn, params, ws, NULL);
        PlanExecutor* exec;
        // Takes ownership of 'ws' and 'cs'.
        Status execStatus = PlanExecutor::make(txn,
                                               ws,
                                               cs,
                                               collection,
                                               PlanExecutor::YIELD_MANUAL,
                                               &exec);
        invariant(execStatus.isOK());
        return exec;
    }

    // static
    PlanExecutor* InternalPlanner::indexScan(OperationContext* txn,
                                             const Collection* collection,
                                             const IndexDescriptor* descriptor,
                                             const BSONObj& startKey, const BSONObj& endKey,
                                             bool endKeyInclusive, Direction direction,
                                             int options) {
        invariant(collection);
        invariant(descriptor);

        IndexScanParams params;
        params.descriptor = descriptor;
        params.direction = direction;
        params.bounds.isSimpleRange = true;
        params.bounds.startKey = startKey;
        params.bounds.endKey = endKey;
        params.bounds.endKeyInclusive = endKeyInclusive;

        WorkingSet* ws = new WorkingSet();
        IndexScan* ix = new IndexScan(txn, params, ws, NULL);

        PlanStage* root = ix;

        if (IXSCAN_FETCH & options) {
            root = new FetchStage(txn, ws, root, NULL, collection);
        }

        PlanExecutor* exec;
        // Takes ownership of 'ws' and 'root'.
        Status execStatus = PlanExecutor::make(txn,
                                               ws,
                                               root,
                                               collection,
                                               PlanExecutor::YIELD_MANUAL,
                                               &exec);
        invariant(execStatus.isOK());
        return exec;
    }

}  // namespace mongo
