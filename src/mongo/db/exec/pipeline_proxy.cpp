/**
 *    Copyright (C) 2014 MongoDB Inc.
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

#include "mongo/db/exec/pipeline_proxy.h"


#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/pipeline_d.h"
#include "mongo/stdx/memory.h"

namespace mongo {

using boost::intrusive_ptr;
using std::shared_ptr;
using std::unique_ptr;
using std::vector;
using stdx::make_unique;

const char* PipelineProxyStage::kStageType = "PIPELINE_PROXY";

PipelineProxyStage::PipelineProxyStage(OperationContext* opCtx,
                                       std::unique_ptr<Pipeline, Pipeline::Deleter> pipeline,
                                       WorkingSet* ws)
    : PlanStage(kStageType, opCtx),
      _pipeline(std::move(pipeline)),
      _includeMetaData(_pipeline->getContext()->needsMerge),  // send metadata to merger
      _includeSortKey(_includeMetaData && !_pipeline->getContext()->from34Mongos),
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
        member->obj = Snapshotted<BSONObj>(SnapshotId(), _stash.back());
        _stash.pop_back();
        member->transitionToOwnedObj();
        return PlanStage::ADVANCED;
    }

    if (boost::optional<BSONObj> next = getNextBson()) {
        *out = _ws->allocate();
        WorkingSetMember* member = _ws->get(*out);
        member->obj = Snapshotted<BSONObj>(SnapshotId(), *next);
        member->transitionToOwnedObj();
        return PlanStage::ADVANCED;
    }

    return PlanStage::IS_EOF;
}

bool PipelineProxyStage::isEOF() {
    if (!_stash.empty())
        return false;

    if (boost::optional<BSONObj> next = getNextBson()) {
        _stash.push_back(*next);
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
        make_unique<PlanStageStats>(CommonStats(kStageType), STAGE_PIPELINE_PROXY);
    ret->specific = make_unique<CollectionScanStats>();
    return ret;
}

boost::optional<BSONObj> PipelineProxyStage::getNextBson() {
    if (auto next = _pipeline->getNext()) {
        if (_includeMetaData) {
            return next->toBsonWithMetaData(_includeSortKey);
        } else {
            return next->toBson();
        }
    }

    return boost::none;
}

Timestamp PipelineProxyStage::getLatestOplogTimestamp() const {
    return PipelineD::getLatestOplogTimestamp(_pipeline.get());
}

std::string PipelineProxyStage::getPlanSummaryStr() const {
    return PipelineD::getPlanSummaryStr(_pipeline.get());
}

void PipelineProxyStage::getPlanSummaryStats(PlanSummaryStats* statsOut) const {
    invariant(statsOut);
    PipelineD::getPlanSummaryStats(_pipeline.get(), statsOut);
    statsOut->nReturned = getCommonStats()->advanced;
}
}  // namespace mongo
