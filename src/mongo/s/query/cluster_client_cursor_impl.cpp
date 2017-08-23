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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kQuery

#include "mongo/platform/basic.h"

#include "mongo/s/query/cluster_client_cursor_impl.h"

#include "mongo/s/query/router_stage_aggregation_merge.h"
#include "mongo/s/query/router_stage_limit.h"
#include "mongo/s/query/router_stage_merge.h"
#include "mongo/s/query/router_stage_mock.h"
#include "mongo/s/query/router_stage_remove_sortkey.h"
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
    : _params(std::move(params)), _root(buildMergerPlan(opCtx, executor, &_params)), _lsid(lsid) {}

ClusterClientCursorImpl::ClusterClientCursorImpl(std::unique_ptr<RouterStageMock> root,
                                                 ClusterClientCursorParams&& params,
                                                 boost::optional<LogicalSessionId> lsid)
    : _params(std::move(params)), _root(std::move(root)), _lsid(lsid) {}

StatusWith<ClusterQueryResult> ClusterClientCursorImpl::next() {
    // First return stashed results, if there are any.
    if (!_stash.empty()) {
        auto front = std::move(_stash.front());
        _stash.pop();
        ++_numReturnedSoFar;
        return {front};
    }

    auto next = _root->next();
    if (next.isOK() && !next.getValue().isEOF()) {
        ++_numReturnedSoFar;
    }
    return next;
}

void ClusterClientCursorImpl::kill(OperationContext* opCtx) {
    _root->kill(opCtx);
}

void ClusterClientCursorImpl::reattachToOperationContext(OperationContext* opCtx) {
    _root->reattachToOperationContext(opCtx);
}

void ClusterClientCursorImpl::detachFromOperationContext() {
    _root->detachFromOperationContext();
}

bool ClusterClientCursorImpl::isTailable() const {
    return _params.isTailable;
}

UserNameIterator ClusterClientCursorImpl::getAuthenticatedUsers() const {
    return makeUserNameIterator(_params.authenticatedUsers.begin(),
                                _params.authenticatedUsers.end());
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

std::unique_ptr<RouterExecStage> ClusterClientCursorImpl::buildMergerPlan(
    OperationContext* opCtx, executor::TaskExecutor* executor, ClusterClientCursorParams* params) {
    const auto skip = params->skip;
    const auto limit = params->limit;
    const bool hasSort = !params->sort.isEmpty();

    // The first stage always merges from the remotes. If 'mergePipeline' has been specified in
    // ClusterClientCursorParams, then RouterStageAggregationMerge should be the root and only node.
    // Otherwise, construct a RouterStage pipeline from the remotes, skip, limit, and sort fields in
    // 'params'.
    if (params->mergePipeline) {
        return stdx::make_unique<RouterStageAggregationMerge>(std::move(params->mergePipeline));
    }

    std::unique_ptr<RouterExecStage> root =
        stdx::make_unique<RouterStageMerge>(opCtx, executor, params);

    if (skip) {
        root = stdx::make_unique<RouterStageSkip>(opCtx, std::move(root), *skip);
    }

    if (limit) {
        root = stdx::make_unique<RouterStageLimit>(opCtx, std::move(root), *limit);
    }

    if (hasSort) {
        root = stdx::make_unique<RouterStageRemoveSortKey>(opCtx, std::move(root));
    }

    return root;
}

}  // namespace mongo
