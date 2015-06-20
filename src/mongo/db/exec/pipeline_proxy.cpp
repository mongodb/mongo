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

namespace mongo {

using boost::intrusive_ptr;
using std::shared_ptr;
using std::vector;

const char* PipelineProxyStage::kStageType = "PIPELINE_PROXY";

PipelineProxyStage::PipelineProxyStage(intrusive_ptr<Pipeline> pipeline,
                                       const std::shared_ptr<PlanExecutor>& child,
                                       WorkingSet* ws)
    : _pipeline(pipeline),
      _includeMetaData(_pipeline->getContext()->inShard)  // send metadata to merger
      ,
      _childExec(child),
      _ws(ws) {}

PlanStage::StageState PipelineProxyStage::work(WorkingSetID* out) {
    if (!out) {
        return PlanStage::FAILURE;
    }

    if (!_stash.empty()) {
        *out = _ws->allocate();
        WorkingSetMember* member = _ws->get(*out);
        member->obj = Snapshotted<BSONObj>(SnapshotId(), _stash.back());
        _stash.pop_back();
        member->state = WorkingSetMember::OWNED_OBJ;
        return PlanStage::ADVANCED;
    }

    if (boost::optional<BSONObj> next = getNextBson()) {
        *out = _ws->allocate();
        WorkingSetMember* member = _ws->get(*out);
        member->obj = Snapshotted<BSONObj>(SnapshotId(), *next);
        member->state = WorkingSetMember::OWNED_OBJ;
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

void PipelineProxyStage::invalidate(OperationContext* txn,
                                    const RecordId& dl,
                                    InvalidationType type) {
    // propagate to child executor if still in use
    if (std::shared_ptr<PlanExecutor> exec = _childExec.lock()) {
        exec->invalidate(txn, dl, type);
    }
}

void PipelineProxyStage::saveState() {
    _pipeline->getContext()->opCtx = NULL;
}

void PipelineProxyStage::restoreState(OperationContext* opCtx) {
    invariant(_pipeline->getContext()->opCtx == NULL);
    _pipeline->getContext()->opCtx = opCtx;
}

void PipelineProxyStage::pushBack(const BSONObj& obj) {
    _stash.push_back(obj);
}

vector<PlanStage*> PipelineProxyStage::getChildren() const {
    vector<PlanStage*> empty;
    return empty;
}

PlanStageStats* PipelineProxyStage::getStats() {
    std::unique_ptr<PlanStageStats> ret(
        new PlanStageStats(CommonStats(kStageType), STAGE_PIPELINE_PROXY));
    ret->specific.reset(new CollectionScanStats());
    return ret.release();
}

boost::optional<BSONObj> PipelineProxyStage::getNextBson() {
    if (boost::optional<Document> next = _pipeline->output()->getNext()) {
        if (_includeMetaData) {
            return next->toBsonWithMetaData();
        } else {
            return next->toBson();
        }
    }

    return boost::none;
}

shared_ptr<PlanExecutor> PipelineProxyStage::getChildExecutor() {
    return _childExec.lock();
}

}  // namespace mongo
