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
#include "mongo/db/exec/delete.h"
#include "mongo/db/exec/eof.h"
#include "mongo/db/exec/fetch.h"
#include "mongo/db/exec/idhack.h"
#include "mongo/db/exec/index_scan.h"
#include "mongo/db/exec/update.h"
#include "mongo/stdx/memory.h"

namespace mongo {

std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> InternalPlanner::collectionScan(
    OperationContext* opCtx,
    StringData ns,
    Collection* collection,
    PlanExecutor::YieldPolicy yieldPolicy,
    const Direction direction,
    const RecordId startLoc) {
    std::unique_ptr<WorkingSet> ws = stdx::make_unique<WorkingSet>();

    if (NULL == collection) {
        auto eof = stdx::make_unique<EOFStage>(opCtx);
        // Takes ownership of 'ws' and 'eof'.
        auto statusWithPlanExecutor = PlanExecutor::make(
            opCtx, std::move(ws), std::move(eof), NamespaceString(ns), yieldPolicy);
        invariant(statusWithPlanExecutor.isOK());
        return std::move(statusWithPlanExecutor.getValue());
    }

    invariant(ns == collection->ns().ns());

    auto cs = _collectionScan(opCtx, ws.get(), collection, direction, startLoc);

    // Takes ownership of 'ws' and 'cs'.
    auto statusWithPlanExecutor =
        PlanExecutor::make(opCtx, std::move(ws), std::move(cs), collection, yieldPolicy);
    invariant(statusWithPlanExecutor.isOK());
    return std::move(statusWithPlanExecutor.getValue());
}

std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> InternalPlanner::deleteWithCollectionScan(
    OperationContext* opCtx,
    Collection* collection,
    const DeleteStageParams& params,
    PlanExecutor::YieldPolicy yieldPolicy,
    Direction direction,
    const RecordId& startLoc) {
    auto ws = stdx::make_unique<WorkingSet>();

    auto root = _collectionScan(opCtx, ws.get(), collection, direction, startLoc);

    root = stdx::make_unique<DeleteStage>(opCtx, params, ws.get(), collection, root.release());

    auto executor =
        PlanExecutor::make(opCtx, std::move(ws), std::move(root), collection, yieldPolicy);
    invariantOK(executor.getStatus());
    return std::move(executor.getValue());
}


std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> InternalPlanner::indexScan(
    OperationContext* opCtx,
    const Collection* collection,
    const IndexDescriptor* descriptor,
    const BSONObj& startKey,
    const BSONObj& endKey,
    BoundInclusion boundInclusion,
    PlanExecutor::YieldPolicy yieldPolicy,
    Direction direction,
    int options) {
    auto ws = stdx::make_unique<WorkingSet>();

    std::unique_ptr<PlanStage> root = _indexScan(opCtx,
                                                 ws.get(),
                                                 collection,
                                                 descriptor,
                                                 startKey,
                                                 endKey,
                                                 boundInclusion,
                                                 direction,
                                                 options);

    auto executor =
        PlanExecutor::make(opCtx, std::move(ws), std::move(root), collection, yieldPolicy);
    invariantOK(executor.getStatus());
    return std::move(executor.getValue());
}

std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> InternalPlanner::deleteWithIndexScan(
    OperationContext* opCtx,
    Collection* collection,
    const DeleteStageParams& params,
    const IndexDescriptor* descriptor,
    const BSONObj& startKey,
    const BSONObj& endKey,
    BoundInclusion boundInclusion,
    PlanExecutor::YieldPolicy yieldPolicy,
    Direction direction) {
    auto ws = stdx::make_unique<WorkingSet>();

    std::unique_ptr<PlanStage> root = _indexScan(opCtx,
                                                 ws.get(),
                                                 collection,
                                                 descriptor,
                                                 startKey,
                                                 endKey,
                                                 boundInclusion,
                                                 direction,
                                                 InternalPlanner::IXSCAN_FETCH);

    root = stdx::make_unique<DeleteStage>(opCtx, params, ws.get(), collection, root.release());

    auto executor =
        PlanExecutor::make(opCtx, std::move(ws), std::move(root), collection, yieldPolicy);
    invariantOK(executor.getStatus());
    return std::move(executor.getValue());
}

std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> InternalPlanner::updateWithIdHack(
    OperationContext* opCtx,
    Collection* collection,
    const UpdateStageParams& params,
    const IndexDescriptor* descriptor,
    const BSONObj& key,
    PlanExecutor::YieldPolicy yieldPolicy) {
    auto ws = stdx::make_unique<WorkingSet>();

    auto idHackStage = stdx::make_unique<IDHackStage>(opCtx, collection, key, ws.get(), descriptor);
    auto root =
        stdx::make_unique<UpdateStage>(opCtx, params, ws.get(), collection, idHackStage.release());

    auto executor =
        PlanExecutor::make(opCtx, std::move(ws), std::move(root), collection, yieldPolicy);
    invariantOK(executor.getStatus());
    return std::move(executor.getValue());
}

std::unique_ptr<PlanStage> InternalPlanner::_collectionScan(OperationContext* opCtx,
                                                            WorkingSet* ws,
                                                            const Collection* collection,
                                                            Direction direction,
                                                            const RecordId& startLoc) {
    invariant(collection);

    CollectionScanParams params;
    params.collection = collection;
    params.start = startLoc;

    if (FORWARD == direction) {
        params.direction = CollectionScanParams::FORWARD;
    } else {
        params.direction = CollectionScanParams::BACKWARD;
    }

    return stdx::make_unique<CollectionScan>(opCtx, params, ws, nullptr);
}

std::unique_ptr<PlanStage> InternalPlanner::_indexScan(OperationContext* opCtx,
                                                       WorkingSet* ws,
                                                       const Collection* collection,
                                                       const IndexDescriptor* descriptor,
                                                       const BSONObj& startKey,
                                                       const BSONObj& endKey,
                                                       BoundInclusion boundInclusion,
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
    params.bounds.boundInclusion = boundInclusion;

    std::unique_ptr<PlanStage> root = stdx::make_unique<IndexScan>(opCtx, params, ws, nullptr);

    if (InternalPlanner::IXSCAN_FETCH & options) {
        root = stdx::make_unique<FetchStage>(opCtx, ws, root.release(), nullptr, collection);
    }

    return root;
}

}  // namespace mongo
