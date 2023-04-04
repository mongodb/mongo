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

#include "mongo/s/query/cluster_client_cursor_impl.h"

#include <memory>

#include "mongo/db/curop.h"
#include "mongo/db/query/telemetry.h"
#include "mongo/logv2/log.h"
#include "mongo/s/query/router_stage_limit.h"
#include "mongo/s/query/router_stage_merge.h"
#include "mongo/s/query/router_stage_remove_metadata_fields.h"
#include "mongo/s/query/router_stage_skip.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {

static CounterMetric mongosCursorStatsTotalOpened("mongos.cursor.totalOpened");
static CounterMetric mongosCursorStatsMoreThanOneBatch("mongos.cursor.moreThanOneBatch");

ClusterClientCursorGuard ClusterClientCursorImpl::make(
    OperationContext* opCtx,
    std::shared_ptr<executor::TaskExecutor> executor,
    ClusterClientCursorParams&& params) {
    std::unique_ptr<ClusterClientCursor> cursor(new ClusterClientCursorImpl(
        opCtx, std::move(executor), std::move(params), opCtx->getLogicalSessionId()));
    return ClusterClientCursorGuard(opCtx, std::move(cursor));
}

ClusterClientCursorGuard ClusterClientCursorImpl::make(OperationContext* opCtx,
                                                       std::unique_ptr<RouterExecStage> root,
                                                       ClusterClientCursorParams&& params) {
    std::unique_ptr<ClusterClientCursor> cursor(new ClusterClientCursorImpl(
        opCtx, std::move(root), std::move(params), opCtx->getLogicalSessionId()));
    return ClusterClientCursorGuard(opCtx, std::move(cursor));
}

ClusterClientCursorImpl::ClusterClientCursorImpl(OperationContext* opCtx,
                                                 std::shared_ptr<executor::TaskExecutor> executor,
                                                 ClusterClientCursorParams&& params,
                                                 boost::optional<LogicalSessionId> lsid)
    : _params(std::move(params)),
      _root(buildMergerPlan(opCtx, std::move(executor), &_params)),
      _lsid(lsid),
      _opCtx(opCtx),
      _createdDate(opCtx->getServiceContext()->getPreciseClockSource()->now()),
      _lastUseDate(_createdDate),
      _queryHash(CurOp::get(opCtx)->debug().queryHash),
      _shouldOmitDiagnosticInformation(CurOp::get(opCtx)->debug().shouldOmitDiagnosticInformation),
      _telemetryStoreKey(CurOp::get(opCtx)->debug().telemetryStoreKey) {
    dassert(!_params.compareWholeSortKeyOnRouter ||
            SimpleBSONObjComparator::kInstance.evaluate(
                _params.sortToApplyOnRouter == AsyncResultsMerger::kWholeSortKeySortPattern));
    mongosCursorStatsTotalOpened.increment();
}

ClusterClientCursorImpl::ClusterClientCursorImpl(OperationContext* opCtx,
                                                 std::unique_ptr<RouterExecStage> root,
                                                 ClusterClientCursorParams&& params,
                                                 boost::optional<LogicalSessionId> lsid)
    : _params(std::move(params)),
      _root(std::move(root)),
      _lsid(lsid),
      _opCtx(opCtx),
      _createdDate(opCtx->getServiceContext()->getPreciseClockSource()->now()),
      _lastUseDate(_createdDate),
      _queryHash(CurOp::get(opCtx)->debug().queryHash),
      _shouldOmitDiagnosticInformation(CurOp::get(opCtx)->debug().shouldOmitDiagnosticInformation) {
    dassert(!_params.compareWholeSortKeyOnRouter ||
            SimpleBSONObjComparator::kInstance.evaluate(
                _params.sortToApplyOnRouter == AsyncResultsMerger::kWholeSortKeySortPattern));
    mongosCursorStatsTotalOpened.increment();
}

ClusterClientCursorImpl::~ClusterClientCursorImpl() {
    if (_metrics.nBatches && *_metrics.nBatches > 1)
        mongosCursorStatsMoreThanOneBatch.increment();
}

StatusWith<ClusterQueryResult> ClusterClientCursorImpl::next() {
    invariant(_opCtx);
    const auto interruptStatus = _opCtx->checkForInterruptNoAssert();
    if (!interruptStatus.isOK()) {
        _maxTimeMSExpired |= (interruptStatus.code() == ErrorCodes::MaxTimeMSExpired);
        return interruptStatus;
    }

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
    // Record if we just got a MaxTimeMSExpired error.
    _maxTimeMSExpired |= (next.getStatus().code() == ErrorCodes::MaxTimeMSExpired);
    return next;
}

void ClusterClientCursorImpl::kill(OperationContext* opCtx) {
    if (_hasBeenKilled) {
        LOGV2_DEBUG(7372700,
                    3,
                    "Kill called on cluster client cursor after cursor has already been killed, so "
                    "ignoring");
        return;
    }

    if (_telemetryStoreKey && opCtx) {
        telemetry::writeTelemetry(opCtx,
                                  _telemetryStoreKey,
                                  getOriginatingCommand(),
                                  _metrics.executionTime.value_or(Microseconds{0}).count(),
                                  _metrics.nreturned.value_or(0));
    }

    _root->kill(opCtx);
    _hasBeenKilled = true;
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

const PrivilegeVector& ClusterClientCursorImpl::getOriginatingPrivileges() const& {
    return _params.originatingPrivileges;
}

bool ClusterClientCursorImpl::partialResultsReturned() const {
    // We may have timed out in this layer, or within the plan tree waiting for results from shards.
    return (_maxTimeMSExpired && _params.isAllowPartialResults) || _root->partialResultsReturned();
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

Date_t ClusterClientCursorImpl::getCreatedDate() const {
    return _createdDate;
}

Date_t ClusterClientCursorImpl::getLastUseDate() const {
    return _lastUseDate;
}

void ClusterClientCursorImpl::setLastUseDate(Date_t now) {
    _lastUseDate = std::move(now);
}

boost::optional<uint32_t> ClusterClientCursorImpl::getQueryHash() const {
    return _queryHash;
}

APIParameters ClusterClientCursorImpl::getAPIParameters() const {
    return _params.apiParameters;
}

boost::optional<ReadPreferenceSetting> ClusterClientCursorImpl::getReadPreference() const {
    return _params.readPreference;
}

boost::optional<repl::ReadConcernArgs> ClusterClientCursorImpl::getReadConcern() const {
    return _params.readConcern;
}

std::unique_ptr<RouterExecStage> ClusterClientCursorImpl::buildMergerPlan(
    OperationContext* opCtx,
    std::shared_ptr<executor::TaskExecutor> executor,
    ClusterClientCursorParams* params) {
    const auto skip = params->skipToApplyOnRouter;
    const auto limit = params->limit;

    std::unique_ptr<RouterExecStage> root =
        std::make_unique<RouterStageMerge>(opCtx, executor, params->extractARMParams());

    if (skip) {
        root = std::make_unique<RouterStageSkip>(opCtx, std::move(root), *skip);
    }

    if (limit) {
        root = std::make_unique<RouterStageLimit>(opCtx, std::move(root), *limit);
    }

    const bool hasSort = !params->sortToApplyOnRouter.isEmpty();
    if (hasSort) {
        // Strip out the sort key after sorting.
        root = std::make_unique<RouterStageRemoveMetadataFields>(
            opCtx, std::move(root), StringDataSet{AsyncResultsMerger::kSortKeyField});
    }

    return root;
}

bool ClusterClientCursorImpl::shouldOmitDiagnosticInformation() const {
    return _shouldOmitDiagnosticInformation;
}

}  // namespace mongo
