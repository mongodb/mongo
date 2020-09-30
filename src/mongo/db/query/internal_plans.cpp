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
#include "mongo/db/query/plan_executor_factory.h"

namespace mongo {

std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> InternalPlanner::collectionScan(
    OperationContext* opCtx,
    StringData ns,
    const CollectionPtr* coll,
    PlanYieldPolicy::YieldPolicy yieldPolicy,
    const Direction direction,
    boost::optional<RecordId> resumeAfterRecordId) {
    const auto& collection = *coll;

    std::unique_ptr<WorkingSet> ws = std::make_unique<WorkingSet>();

    auto expCtx = make_intrusive<ExpressionContext>(
        opCtx, std::unique_ptr<CollatorInterface>(nullptr), NamespaceString(ns));

    if (!collection) {
        auto eof = std::make_unique<EOFStage>(expCtx.get());
        // Takes ownership of 'ws' and 'eof'.
        auto statusWithPlanExecutor = plan_executor_factory::make(expCtx,
                                                                  std::move(ws),
                                                                  std::move(eof),
                                                                  &CollectionPtr::null,
                                                                  yieldPolicy,
                                                                  NamespaceString(ns));
        invariant(statusWithPlanExecutor.isOK());
        return std::move(statusWithPlanExecutor.getValue());
    }

    invariant(ns == collection->ns().ns());

    auto cs = _collectionScan(expCtx, ws.get(), &collection, direction, resumeAfterRecordId);

    // Takes ownership of 'ws' and 'cs'.
    auto statusWithPlanExecutor =
        plan_executor_factory::make(expCtx, std::move(ws), std::move(cs), &collection, yieldPolicy);
    invariant(statusWithPlanExecutor.isOK());
    return std::move(statusWithPlanExecutor.getValue());
}

std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> InternalPlanner::deleteWithCollectionScan(
    OperationContext* opCtx,
    const CollectionPtr* coll,
    std::unique_ptr<DeleteStageParams> params,
    PlanYieldPolicy::YieldPolicy yieldPolicy,
    Direction direction) {
    const auto& collection = *coll;
    invariant(collection);
    auto ws = std::make_unique<WorkingSet>();

    auto expCtx = make_intrusive<ExpressionContext>(
        opCtx, std::unique_ptr<CollatorInterface>(nullptr), collection->ns());

    auto root = _collectionScan(expCtx, ws.get(), &collection, direction);

    root = std::make_unique<DeleteStage>(
        expCtx.get(), std::move(params), ws.get(), collection, root.release());

    auto executor = plan_executor_factory::make(
        expCtx, std::move(ws), std::move(root), &collection, yieldPolicy);
    invariant(executor.getStatus());
    return std::move(executor.getValue());
}


std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> InternalPlanner::indexScan(
    OperationContext* opCtx,
    const CollectionPtr* coll,
    const IndexDescriptor* descriptor,
    const BSONObj& startKey,
    const BSONObj& endKey,
    BoundInclusion boundInclusion,
    PlanYieldPolicy::YieldPolicy yieldPolicy,
    Direction direction,
    int options) {
    const auto& collection = *coll;
    auto ws = std::make_unique<WorkingSet>();

    auto expCtx = make_intrusive<ExpressionContext>(
        opCtx, std::unique_ptr<CollatorInterface>(nullptr), collection->ns());

    std::unique_ptr<PlanStage> root = _indexScan(expCtx,
                                                 ws.get(),
                                                 &collection,
                                                 descriptor,
                                                 startKey,
                                                 endKey,
                                                 boundInclusion,
                                                 direction,
                                                 options);

    auto executor = plan_executor_factory::make(
        expCtx, std::move(ws), std::move(root), &collection, yieldPolicy);
    invariant(executor.getStatus());
    return std::move(executor.getValue());
}

std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> InternalPlanner::deleteWithIndexScan(
    OperationContext* opCtx,
    const CollectionPtr* coll,
    std::unique_ptr<DeleteStageParams> params,
    const IndexDescriptor* descriptor,
    const BSONObj& startKey,
    const BSONObj& endKey,
    BoundInclusion boundInclusion,
    PlanYieldPolicy::YieldPolicy yieldPolicy,
    Direction direction) {
    const auto& collection = *coll;
    invariant(collection);
    auto ws = std::make_unique<WorkingSet>();

    auto expCtx = make_intrusive<ExpressionContext>(
        opCtx, std::unique_ptr<CollatorInterface>(nullptr), collection->ns());

    std::unique_ptr<PlanStage> root = _indexScan(expCtx,
                                                 ws.get(),
                                                 &collection,
                                                 descriptor,
                                                 startKey,
                                                 endKey,
                                                 boundInclusion,
                                                 direction,
                                                 InternalPlanner::IXSCAN_FETCH);

    root = std::make_unique<DeleteStage>(
        expCtx.get(), std::move(params), ws.get(), collection, root.release());

    auto executor = plan_executor_factory::make(
        expCtx, std::move(ws), std::move(root), &collection, yieldPolicy);
    invariant(executor.getStatus());
    return std::move(executor.getValue());
}

std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> InternalPlanner::updateWithIdHack(
    OperationContext* opCtx,
    const CollectionPtr* coll,
    const UpdateStageParams& params,
    const IndexDescriptor* descriptor,
    const BSONObj& key,
    PlanYieldPolicy::YieldPolicy yieldPolicy) {
    const auto& collection = *coll;
    invariant(collection);
    auto ws = std::make_unique<WorkingSet>();

    auto expCtx = make_intrusive<ExpressionContext>(
        opCtx, std::unique_ptr<CollatorInterface>(nullptr), collection->ns());

    auto idHackStage =
        std::make_unique<IDHackStage>(expCtx.get(), key, ws.get(), collection, descriptor);

    const bool isUpsert = params.request->isUpsert();
    auto root = (isUpsert ? std::make_unique<UpsertStage>(
                                expCtx.get(), params, ws.get(), collection, idHackStage.release())
                          : std::make_unique<UpdateStage>(
                                expCtx.get(), params, ws.get(), collection, idHackStage.release()));

    auto executor = plan_executor_factory::make(
        expCtx, std::move(ws), std::move(root), &collection, yieldPolicy);
    invariant(executor.getStatus());
    return std::move(executor.getValue());
}

std::unique_ptr<PlanStage> InternalPlanner::_collectionScan(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    WorkingSet* ws,
    const CollectionPtr* coll,
    Direction direction,
    boost::optional<RecordId> resumeAfterRecordId) {
    const auto& collection = *coll;
    invariant(collection);

    CollectionScanParams params;
    params.shouldWaitForOplogVisibility =
        shouldWaitForOplogVisibility(expCtx->opCtx, collection, false);
    params.resumeAfterRecordId = resumeAfterRecordId;

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
    const CollectionPtr* coll,
    const IndexDescriptor* descriptor,
    const BSONObj& startKey,
    const BSONObj& endKey,
    BoundInclusion boundInclusion,
    Direction direction,
    int options) {
    const auto& collection = *coll;
    invariant(collection);
    invariant(descriptor);

    IndexScanParams params(expCtx->opCtx, descriptor);
    params.direction = direction;
    params.bounds.isSimpleRange = true;
    params.bounds.startKey = startKey;
    params.bounds.endKey = endKey;
    params.bounds.boundInclusion = boundInclusion;
    params.shouldDedup = descriptor->getEntry()->isMultikey();

    std::unique_ptr<PlanStage> root =
        std::make_unique<IndexScan>(expCtx.get(), collection, std::move(params), ws, nullptr);

    if (InternalPlanner::IXSCAN_FETCH & options) {
        root = std::make_unique<FetchStage>(expCtx.get(), ws, std::move(root), nullptr, collection);
    }

    return root;
}

}  // namespace mongo
