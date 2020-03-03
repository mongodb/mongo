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

#include "mongo/platform/basic.h"

#include "mongo/db/query/internal_plans.h"

#include <memory>

#include "mongo/db/catalog/database.h"
#include "mongo/db/client.h"
#include "mongo/db/exec/collection_scan.h"
#include "mongo/db/exec/delete.h"
#include "mongo/db/exec/eof.h"
#include "mongo/db/exec/fetch.h"
#include "mongo/db/exec/idhack.h"
#include "mongo/db/exec/index_scan.h"
#include "mongo/db/exec/update_stage.h"
#include "mongo/db/exec/upsert_stage.h"
#include "mongo/db/query/get_executor.h"

namespace mongo {

std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> InternalPlanner::collectionScan(
    OperationContext* opCtx,
    StringData ns,
    Collection* collection,
    PlanExecutor::YieldPolicy yieldPolicy,
    const Direction direction) {
    std::unique_ptr<WorkingSet> ws = std::make_unique<WorkingSet>();

    auto expCtx = make_intrusive<ExpressionContext>(
        opCtx, std::unique_ptr<CollatorInterface>(nullptr), NamespaceString(ns));

    if (nullptr == collection) {
        auto eof = std::make_unique<EOFStage>(expCtx.get());
        // Takes ownership of 'ws' and 'eof'.
        auto statusWithPlanExecutor = PlanExecutor::make(
            expCtx, std::move(ws), std::move(eof), nullptr, yieldPolicy, NamespaceString(ns));
        invariant(statusWithPlanExecutor.isOK());
        return std::move(statusWithPlanExecutor.getValue());
    }

    invariant(ns == collection->ns().ns());

    auto cs = _collectionScan(expCtx, ws.get(), collection, direction);

    // Takes ownership of 'ws' and 'cs'.
    auto statusWithPlanExecutor =
        PlanExecutor::make(expCtx, std::move(ws), std::move(cs), collection, yieldPolicy);
    invariant(statusWithPlanExecutor.isOK());
    return std::move(statusWithPlanExecutor.getValue());
}

std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> InternalPlanner::deleteWithCollectionScan(
    OperationContext* opCtx,
    Collection* collection,
    std::unique_ptr<DeleteStageParams> params,
    PlanExecutor::YieldPolicy yieldPolicy,
    Direction direction) {
    invariant(collection);
    auto ws = std::make_unique<WorkingSet>();

    auto expCtx = make_intrusive<ExpressionContext>(
        opCtx, std::unique_ptr<CollatorInterface>(nullptr), collection->ns());

    auto root = _collectionScan(expCtx, ws.get(), collection, direction);

    root = std::make_unique<DeleteStage>(
        expCtx.get(), std::move(params), ws.get(), collection, root.release());

    auto executor =
        PlanExecutor::make(expCtx, std::move(ws), std::move(root), collection, yieldPolicy);
    invariant(executor.getStatus());
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
    auto ws = std::make_unique<WorkingSet>();

    auto expCtx = make_intrusive<ExpressionContext>(
        opCtx, std::unique_ptr<CollatorInterface>(nullptr), collection->ns());

    std::unique_ptr<PlanStage> root = _indexScan(expCtx,
                                                 ws.get(),
                                                 collection,
                                                 descriptor,
                                                 startKey,
                                                 endKey,
                                                 boundInclusion,
                                                 direction,
                                                 options);

    auto executor =
        PlanExecutor::make(expCtx, std::move(ws), std::move(root), collection, yieldPolicy);
    invariant(executor.getStatus());
    return std::move(executor.getValue());
}

std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> InternalPlanner::deleteWithIndexScan(
    OperationContext* opCtx,
    Collection* collection,
    std::unique_ptr<DeleteStageParams> params,
    const IndexDescriptor* descriptor,
    const BSONObj& startKey,
    const BSONObj& endKey,
    BoundInclusion boundInclusion,
    PlanExecutor::YieldPolicy yieldPolicy,
    Direction direction) {
    invariant(collection);
    auto ws = std::make_unique<WorkingSet>();

    auto expCtx = make_intrusive<ExpressionContext>(
        opCtx, std::unique_ptr<CollatorInterface>(nullptr), collection->ns());

    std::unique_ptr<PlanStage> root = _indexScan(expCtx,
                                                 ws.get(),
                                                 collection,
                                                 descriptor,
                                                 startKey,
                                                 endKey,
                                                 boundInclusion,
                                                 direction,
                                                 InternalPlanner::IXSCAN_FETCH);

    root = std::make_unique<DeleteStage>(
        expCtx.get(), std::move(params), ws.get(), collection, root.release());

    auto executor =
        PlanExecutor::make(expCtx, std::move(ws), std::move(root), collection, yieldPolicy);
    invariant(executor.getStatus());
    return std::move(executor.getValue());
}

std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> InternalPlanner::updateWithIdHack(
    OperationContext* opCtx,
    Collection* collection,
    const UpdateStageParams& params,
    const IndexDescriptor* descriptor,
    const BSONObj& key,
    PlanExecutor::YieldPolicy yieldPolicy) {
    invariant(collection);
    auto ws = std::make_unique<WorkingSet>();

    auto expCtx = make_intrusive<ExpressionContext>(
        opCtx, std::unique_ptr<CollatorInterface>(nullptr), collection->ns());

    auto idHackStage = std::make_unique<IDHackStage>(expCtx.get(), key, ws.get(), descriptor);

    const bool isUpsert = params.request->isUpsert();
    auto root = (isUpsert ? std::make_unique<UpsertStage>(
                                expCtx.get(), params, ws.get(), collection, idHackStage.release())
                          : std::make_unique<UpdateStage>(
                                expCtx.get(), params, ws.get(), collection, idHackStage.release()));

    auto executor =
        PlanExecutor::make(expCtx, std::move(ws), std::move(root), collection, yieldPolicy);
    invariant(executor.getStatus());
    return std::move(executor.getValue());
}

std::unique_ptr<PlanStage> InternalPlanner::_collectionScan(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    WorkingSet* ws,
    const Collection* collection,
    Direction direction) {
    invariant(collection);

    CollectionScanParams params;
    params.shouldWaitForOplogVisibility =
        shouldWaitForOplogVisibility(expCtx->opCtx, collection, false);

    if (FORWARD == direction) {
        params.direction = CollectionScanParams::FORWARD;
    } else {
        params.direction = CollectionScanParams::BACKWARD;
    }

    return std::make_unique<CollectionScan>(expCtx.get(), collection, params, ws, nullptr);
}

std::unique_ptr<PlanStage> InternalPlanner::_indexScan(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
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

    IndexScanParams params(expCtx->opCtx, descriptor);
    params.direction = direction;
    params.bounds.isSimpleRange = true;
    params.bounds.startKey = startKey;
    params.bounds.endKey = endKey;
    params.bounds.boundInclusion = boundInclusion;
    params.shouldDedup = descriptor->isMultikey();

    std::unique_ptr<PlanStage> root =
        std::make_unique<IndexScan>(expCtx.get(), std::move(params), ws, nullptr);

    if (InternalPlanner::IXSCAN_FETCH & options) {
        root = std::make_unique<FetchStage>(expCtx.get(), ws, std::move(root), nullptr, collection);
    }

    return root;
}

}  // namespace mongo
