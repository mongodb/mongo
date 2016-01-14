/**
 *    Copyright (C) 2013 10gen Inc.
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

#include "mongo/db/exec/queued_data_stage.h"

#include "mongo/db/exec/scoped_timer.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/stdx/memory.h"

namespace mongo {

using std::unique_ptr;
using std::vector;
using stdx::make_unique;

const char* QueuedDataStage::kStageType = "QUEUED_DATA";

QueuedDataStage::QueuedDataStage(OperationContext* opCtx, WorkingSet* ws)
    : PlanStage(kStageType, opCtx), _ws(ws) {}

PlanStage::StageState QueuedDataStage::doWork(WorkingSetID* out) {
    if (isEOF()) {
        return PlanStage::IS_EOF;
    }

    StageState state = _results.front();
    _results.pop();

    if (PlanStage::ADVANCED == state) {
        *out = _members.front();
        _members.pop();
    }

    return state;
}

bool QueuedDataStage::isEOF() {
    return _results.empty();
}

unique_ptr<PlanStageStats> QueuedDataStage::getStats() {
    _commonStats.isEOF = isEOF();
    unique_ptr<PlanStageStats> ret = make_unique<PlanStageStats>(_commonStats, STAGE_QUEUED_DATA);
    ret->specific = make_unique<MockStats>(_specificStats);
    return ret;
}


const SpecificStats* QueuedDataStage::getSpecificStats() const {
    return &_specificStats;
}

void QueuedDataStage::pushBack(const PlanStage::StageState state) {
    invariant(PlanStage::ADVANCED != state);
    _results.push(state);
}

void QueuedDataStage::pushBack(const WorkingSetID& id) {
    _results.push(PlanStage::ADVANCED);

    // member lives in _ws.  We'll return it when _results hits ADVANCED.
    _members.push(id);
}

}  // namespace mongo
