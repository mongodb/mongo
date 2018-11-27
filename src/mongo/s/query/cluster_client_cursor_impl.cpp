
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

#include "mongo/s/query/cluster_client_cursor_impl.h"

#include "mongo/db/pipeline/cluster_aggregation_planner.h"
#include "mongo/db/pipeline/document_source_limit.h"
#include "mongo/db/pipeline/document_source_skip.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/s/query/router_stage_limit.h"
#include "mongo/s/query/router_stage_merge.h"
#include "mongo/s/query/router_stage_mock.h"
#include "mongo/s/query/router_stage_pipeline.h"
#include "mongo/s/query/router_stage_remove_metadata_fields.h"
#include "mongo/s/query/router_stage_skip.h"
#include "mongo/stdx/memory.h"

namespace mongo {

ClusterClientCursorGuard::ClusterClientCursorGuard(OperationContext* opCtx,
                                                   std::unique_ptr<ClusterClientCursor> ccc)
    : _opCtx(opCtx), _ccc(std::move(ccc)) {}

ClusterClientCursorGuard::~ClusterClientCursorGuard() {
    if (_ccc && !_ccc->remotesExhausted()) {
        _ccc->kill(_opCtx);
    }
}

ClusterClientCursor* ClusterClientCursorGuard::operator->() {
    return _ccc.get();
}

std::unique_ptr<ClusterClientCursor> ClusterClientCursorGuard::releaseCursor() {
    return std::move(_ccc);
}

ClusterClientCursorGuard ClusterClientCursorImpl::make(OperationContext* opCtx,
                                                       executor::TaskExecutor* executor,
                                                       ClusterClientCursorParams&& params) {
    std::unique_ptr<ClusterClientCursor> cursor(new ClusterClientCursorImpl(
        opCtx, executor, std::move(params), opCtx->getLogicalSessionId()));
    return ClusterClientCursorGuard(opCtx, std::move(cursor));
}

ClusterClientCursorImpl::ClusterClientCursorImpl(OperationContext* opCtx,
                                                 executor::TaskExecutor* executor,
                                                 ClusterClientCursorParams&& params,
                                                 boost::optional<LogicalSessionId> lsid)
    : _params(std::move(params)),
      _root(buildMergerPlan(opCtx, executor, &_params)),
      _lsid(lsid),
      _opCtx(opCtx) {
    dassert(!_params.compareWholeSortKey ||
            SimpleBSONObjComparator::kInstance.evaluate(
                _params.sort == AsyncResultsMerger::kWholeSortKeySortPattern));
}

ClusterClientCursorImpl::ClusterClientCursorImpl(OperationContext* opCtx,
                                                 std::unique_ptr<RouterStageMock> root,
                                                 ClusterClientCursorParams&& params,
                                                 boost::optional<LogicalSessionId> lsid)
    : _params(std::move(params)), _root(std::move(root)), _lsid(lsid), _opCtx(opCtx) {
    dassert(!_params.compareWholeSortKey ||
            SimpleBSONObjComparator::kInstance.evaluate(
                _params.sort == AsyncResultsMerger::kWholeSortKeySortPattern));
}

StatusWith<ClusterQueryResult> ClusterClientCursorImpl::next(
    RouterExecStage::ExecContext execContext) {

    invariant(_opCtx);
    const auto interruptStatus = _opCtx->checkForInterruptNoAssert();
    if (!interruptStatus.isOK()) {
        return interruptStatus;
    }

    // First return stashed results, if there are any.
    if (!_stash.empty()) {
        auto front = std::move(_stash.front());
        _stash.pop();
        ++_numReturnedSoFar;
        return {front};
    }

    auto next = _root->next(execContext);
    if (next.isOK() && !next.getValue().isEOF()) {
        ++_numReturnedSoFar;
    }
    return next;
}

void ClusterClientCursorImpl::kill(OperationContext* opCtx) {
    _root->kill(opCtx);
}

void ClusterClientCursorImpl::reattachToOperationContext(OperationContext* opCtx) {
    _opCtx = opCtx;
    _root->reattachToOperationContext(opCtx);
}

void ClusterClientCursorImpl::detachFromOperationContext() {
    _opCtx = nullptr;
    _root->detachFromOperationContext();
}

OperationContext* ClusterClientCursorImpl::getCurrentOperationContext() const {
    return _opCtx;
}

bool ClusterClientCursorImpl::isTailable() const {
    return _params.tailableMode != TailableModeEnum::kNormal;
}

bool ClusterClientCursorImpl::isTailableAndAwaitData() const {
    return _params.tailableMode == TailableModeEnum::kTailableAndAwaitData;
}

BSONObj ClusterClientCursorImpl::getOriginatingCommand() const {
    return _params.originatingCommandObj;
}

std::size_t ClusterClientCursorImpl::getNumRemotes() const {
    return _root->getNumRemotes();
}

BSONObj ClusterClientCursorImpl::getPostBatchResumeToken() const {
    return _root->getPostBatchResumeToken();
}

long long ClusterClientCursorImpl::getNumReturnedSoFar() const {
    return _numReturnedSoFar;
}

void ClusterClientCursorImpl::queueResult(const ClusterQueryResult& result) {
    auto resultObj = result.getResult();
    if (resultObj) {
        invariant(resultObj->isOwned());
    }
    _stash.push(result);
}

bool ClusterClientCursorImpl::remotesExhausted() {
    return _root->remotesExhausted();
}

Status ClusterClientCursorImpl::setAwaitDataTimeout(Milliseconds awaitDataTimeout) {
    return _root->setAwaitDataTimeout(awaitDataTimeout);
}

boost::optional<LogicalSessionId> ClusterClientCursorImpl::getLsid() const {
    return _lsid;
}

boost::optional<TxnNumber> ClusterClientCursorImpl::getTxnNumber() const {
    return _params.txnNumber;
}

boost::optional<ReadPreferenceSetting> ClusterClientCursorImpl::getReadPreference() const {
    return _params.readPreference;
}

namespace {

bool isSkipOrLimit(const boost::intrusive_ptr<DocumentSource>& stage) {
    return (dynamic_cast<DocumentSourceLimit*>(stage.get()) ||
            dynamic_cast<DocumentSourceSkip*>(stage.get()));
}

bool isAllLimitsAndSkips(Pipeline* pipeline) {
    const auto stages = pipeline->getSources();
    return std::all_of(
        stages.begin(), stages.end(), [&](const auto& stage) { return isSkipOrLimit(stage); });
}

/**
 * Creates the initial stage to feed data into the execution plan.  By default, a RouterExecMerge
 * stage, or a custom stage if specified in 'params->creatCustomMerge'.
 */
std::unique_ptr<RouterExecStage> createInitialStage(OperationContext* opCtx,
                                                    executor::TaskExecutor* executor,
                                                    ClusterClientCursorParams* params) {
    if (params->createCustomCursorSource) {
        return params->createCustomCursorSource(opCtx, executor, params);
    } else {
        return stdx::make_unique<RouterStageMerge>(opCtx, executor, params);
    }
}

std::unique_ptr<RouterExecStage> buildPipelinePlan(executor::TaskExecutor* executor,
                                                   ClusterClientCursorParams* params) {
    invariant(params->mergePipeline);
    invariant(!params->skip);
    invariant(!params->limit);
    auto* pipeline = params->mergePipeline.get();
    auto* opCtx = pipeline->getContext()->opCtx;

    std::unique_ptr<RouterExecStage> root = createInitialStage(opCtx, executor, params);
    if (!isAllLimitsAndSkips(pipeline)) {
        return stdx::make_unique<RouterStagePipeline>(std::move(root),
                                                      std::move(params->mergePipeline));
    }

    // After extracting an optional leading $sort, the pipeline consisted entirely of $skip and
    // $limit stages. Avoid creating a RouterStagePipeline (which will go through an expensive
    // conversion from BSONObj -> Document for each result), and create a RouterExecStage tree
    // instead.
    while (!pipeline->getSources().empty()) {
        invariant(isSkipOrLimit(pipeline->getSources().front()));
        if (auto skip = pipeline->popFrontWithName(DocumentSourceSkip::kStageName)) {
            root = stdx::make_unique<RouterStageSkip>(
                opCtx, std::move(root), static_cast<DocumentSourceSkip*>(skip.get())->getSkip());
        } else if (auto limit = pipeline->popFrontWithName(DocumentSourceLimit::kStageName)) {
            root = stdx::make_unique<RouterStageLimit>(
                opCtx, std::move(root), static_cast<DocumentSourceLimit*>(limit.get())->getLimit());
        }
    }
    // We are executing the pipeline without using an actual Pipeline, so we need to strip out any
    // Document metadata ourselves.
    return stdx::make_unique<RouterStageRemoveMetadataFields>(
        opCtx, std::move(root), Document::allMetadataFieldNames);
}
}  // namespace

std::unique_ptr<RouterExecStage> ClusterClientCursorImpl::buildMergerPlan(
    OperationContext* opCtx, executor::TaskExecutor* executor, ClusterClientCursorParams* params) {
    const auto skip = params->skip;
    const auto limit = params->limit;
    if (params->mergePipeline) {
        if (auto sort =
                cluster_aggregation_planner::popLeadingMergeSort(params->mergePipeline.get())) {
            params->sort = *sort;
        }
        return buildPipelinePlan(executor, params);
    }

    std::unique_ptr<RouterExecStage> root = createInitialStage(opCtx, executor, params);

    if (skip) {
        root = stdx::make_unique<RouterStageSkip>(opCtx, std::move(root), *skip);
    }

    if (limit) {
        root = stdx::make_unique<RouterStageLimit>(opCtx, std::move(root), *limit);
    }

    const bool hasSort = !params->sort.isEmpty();
    if (hasSort) {
        // Strip out the sort key after sorting.
        root = stdx::make_unique<RouterStageRemoveMetadataFields>(
            opCtx, std::move(root), std::vector<StringData>{AsyncResultsMerger::kSortKeyField});
    }

    return root;
}

}  // namespace mongo
