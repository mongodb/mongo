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
#include "mongo/stdx/memory.h"

namespace mongo {

// static
std::unique_ptr<PlanExecutor> InternalPlanner::collectionScan(OperationContext* txn,
                                                              StringData ns,
                                                              Collection* collection,
                                                              const Direction direction,
                                                              const RecordId startLoc) {
    std::unique_ptr<WorkingSet> ws = stdx::make_unique<WorkingSet>();

    if (NULL == collection) {
        auto eof = stdx::make_unique<EOFStage>(txn);
        // Takes ownership of 'ws' and 'eof'.
        auto statusWithPlanExecutor = PlanExecutor::make(
            txn, std::move(ws), std::move(eof), ns.toString(), PlanExecutor::YIELD_MANUAL);
        invariant(statusWithPlanExecutor.isOK());
        return std::move(statusWithPlanExecutor.getValue());
    }

    invariant(ns == collection->ns().ns());

    CollectionScanParams params;
    params.collection = collection;
    params.start = startLoc;

    if (FORWARD == direction) {
        params.direction = CollectionScanParams::FORWARD;
    } else {
        params.direction = CollectionScanParams::BACKWARD;
    }

    std::unique_ptr<CollectionScan> cs =
        stdx::make_unique<CollectionScan>(txn, params, ws.get(), nullptr);
    // Takes ownership of 'ws' and 'cs'.
    auto statusWithPlanExecutor = PlanExecutor::make(
        txn, std::move(ws), std::move(cs), collection, PlanExecutor::YIELD_MANUAL);
    invariant(statusWithPlanExecutor.isOK());
    return std::move(statusWithPlanExecutor.getValue());
}

// static
std::unique_ptr<PlanExecutor> InternalPlanner::indexScan(OperationContext* txn,
                                                         const Collection* collection,
                                                         const IndexDescriptor* descriptor,
                                                         const BSONObj& startKey,
                                                         const BSONObj& endKey,
                                                         bool endKeyInclusive,
                                                         Direction direction,
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

    std::unique_ptr<WorkingSet> ws = stdx::make_unique<WorkingSet>();

    std::unique_ptr<PlanStage> root = stdx::make_unique<IndexScan>(txn, params, ws.get(), nullptr);

    if (IXSCAN_FETCH & options) {
        root = stdx::make_unique<FetchStage>(txn, ws.get(), root.release(), nullptr, collection);
    }

    // Takes ownership of 'ws' and 'root'.
    auto statusWithPlanExecutor = PlanExecutor::make(
        txn, std::move(ws), std::move(root), collection, PlanExecutor::YIELD_MANUAL);
    invariant(statusWithPlanExecutor.isOK());
    return std::move(statusWithPlanExecutor.getValue());
}

}  // namespace mongo
