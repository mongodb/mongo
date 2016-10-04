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

#include "mongo/db/exec/skip.h"
#include "mongo/db/exec/scoped_timer.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

using std::unique_ptr;
using std::vector;
using stdx::make_unique;

// static
const char* SkipStage::kStageType = "SKIP";

SkipStage::SkipStage(OperationContext* opCtx, long long toSkip, WorkingSet* ws, PlanStage* child)
    : PlanStage(kStageType, opCtx), _ws(ws), _toSkip(toSkip) {
    _children.emplace_back(child);
}

SkipStage::~SkipStage() {}

bool SkipStage::isEOF() {
    return child()->isEOF();
}

PlanStage::StageState SkipStage::doWork(WorkingSetID* out) {
    WorkingSetID id = WorkingSet::INVALID_ID;
    StageState status = child()->work(&id);

    if (PlanStage::ADVANCED == status) {
        // If we're still skipping results...
        if (_toSkip > 0) {
            // ...drop the result.
            --_toSkip;
            _ws->free(id);
            return PlanStage::NEED_TIME;
        }

        *out = id;
        return PlanStage::ADVANCED;
    } else if (PlanStage::FAILURE == status || PlanStage::DEAD == status) {
        *out = id;
        // If a stage fails, it may create a status WSM to indicate why it
        // failed, in which case 'id' is valid.  If ID is invalid, we
        // create our own error message.
        if (WorkingSet::INVALID_ID == id) {
            mongoutils::str::stream ss;
            ss << "skip stage failed to read in results from child";
            Status status(ErrorCodes::InternalError, ss);
            *out = WorkingSetCommon::allocateStatusMember(_ws, status);
        }
        return status;
    } else if (PlanStage::NEED_YIELD == status) {
        *out = id;
    }

    // NEED_TIME, NEED_YIELD, ERROR, IS_EOF
    return status;
}

unique_ptr<PlanStageStats> SkipStage::getStats() {
    _commonStats.isEOF = isEOF();
    _specificStats.skip = _toSkip;
    unique_ptr<PlanStageStats> ret = make_unique<PlanStageStats>(_commonStats, STAGE_SKIP);
    ret->specific = make_unique<SkipStats>(_specificStats);
    ret->children.emplace_back(child()->getStats());
    return ret;
}

const SpecificStats* SkipStage::getSpecificStats() const {
    return &_specificStats;
}

}  // namespace mongo
