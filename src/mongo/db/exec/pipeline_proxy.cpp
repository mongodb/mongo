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

#include "mongo/db/exec/pipeline_proxy.h"

#include <memory>

#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/pipeline_d.h"

namespace mongo {

using boost::intrusive_ptr;
using std::shared_ptr;
using std::unique_ptr;
using std::vector;

const char* PipelineProxyStage::kStageType = "PIPELINE_PROXY";

PipelineProxyStage::PipelineProxyStage(OperationContext* opCtx,
                                       std::unique_ptr<Pipeline, PipelineDeleter> pipeline,
                                       WorkingSet* ws)
    : PipelineProxyStage(opCtx, std::move(pipeline), ws, kStageType) {}

PipelineProxyStage::PipelineProxyStage(OperationContext* opCtx,
                                       std::unique_ptr<Pipeline, PipelineDeleter> pipeline,
                                       WorkingSet* ws,
                                       const char* stageTypeName)
    : PlanStage(stageTypeName, opCtx),
      _pipeline(std::move(pipeline)),
      _includeMetaData(_pipeline->getContext()->needsMerge),  // send metadata to merger
      _ws(ws) {
    // We take over responsibility for disposing of the Pipeline, since it is required that
    // doDispose() will be called before destruction of this PipelineProxyStage.
    _pipeline.get_deleter().dismissDisposal();
}

PlanStage::StageState PipelineProxyStage::doWork(WorkingSetID* out) {
    if (!out) {
        return PlanStage::FAILURE;
    }

    if (!_stash.empty()) {
        *out = _ws->allocate();
        WorkingSetMember* member = _ws->get(*out);
        if (_includeMetaData && _stash.back().metadata()) {
            member->metadata() = _stash.back().metadata();
        }
        member->doc = {SnapshotId(), std::move(_stash.back())};
        _stash.pop_back();
        member->transitionToOwnedObj();
        return PlanStage::ADVANCED;
    }

    if (auto next = getNext()) {
        *out = _ws->allocate();
        WorkingSetMember* member = _ws->get(*out);
        if (_includeMetaData && next->metadata()) {
            member->metadata() = next->metadata();
        }
        member->doc = {SnapshotId(), std::move(*next)};
        member->transitionToOwnedObj();
        return PlanStage::ADVANCED;
    }

    return PlanStage::IS_EOF;
}

bool PipelineProxyStage::isEOF() {
    if (!_stash.empty())
        return false;

    if (auto next = getNext()) {
        _stash.emplace_back(*next);
        return false;
    }

    return true;
}

void PipelineProxyStage::doDetachFromOperationContext() {
    _pipeline->detachFromOperationContext();
}

void PipelineProxyStage::doReattachToOperationContext() {
    _pipeline->reattachToOperationContext(getOpCtx());
}

void PipelineProxyStage::doDispose() {
    _pipeline->dispose(getOpCtx());
}

unique_ptr<PlanStageStats> PipelineProxyStage::getStats() {
    unique_ptr<PlanStageStats> ret =
        std::make_unique<PlanStageStats>(CommonStats(kStageType), STAGE_PIPELINE_PROXY);
    ret->specific = std::make_unique<CollectionScanStats>();
    return ret;
}

boost::optional<Document> PipelineProxyStage::getNext() {
    return _pipeline->getNext();
}

std::string PipelineProxyStage::getPlanSummaryStr() const {
    return PipelineD::getPlanSummaryStr(_pipeline.get());
}

void PipelineProxyStage::getPlanSummaryStats(PlanSummaryStats* statsOut) const {
    invariant(statsOut);
    PipelineD::getPlanSummaryStats(_pipeline.get(), statsOut);
    statsOut->nReturned = getCommonStats()->advanced;
}

vector<Value> PipelineProxyStage::writeExplainOps(ExplainOptions::Verbosity verbosity) const {
    return _pipeline->writeExplainOps(verbosity);
}

}  // namespace mongo
