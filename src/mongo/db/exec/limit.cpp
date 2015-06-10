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

#include "mongo/db/exec/limit.h"

#include "mongo/db/exec/scoped_timer.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    using std::unique_ptr;
    using std::vector;

    // static
    const char* LimitStage::kStageType = "LIMIT";

    LimitStage::LimitStage(int limit, WorkingSet* ws, PlanStage* child)
        : _ws(ws),
          _child(child),
          _numToReturn(limit),
          _commonStats(kStageType) {
        _specificStats.limit = _numToReturn;
    }

    LimitStage::~LimitStage() { }

    bool LimitStage::isEOF() { return (0 == _numToReturn) || _child->isEOF(); }

    PlanStage::StageState LimitStage::work(WorkingSetID* out) {
        ++_commonStats.works;

        // Adds the amount of time taken by work() to executionTimeMillis.
        ScopedTimer timer(&_commonStats.executionTimeMillis);

        if (0 == _numToReturn) {
            // We've returned as many results as we're limited to.
            return PlanStage::IS_EOF;
        }

        WorkingSetID id = WorkingSet::INVALID_ID;
        StageState status = _child->work(&id);

        if (PlanStage::ADVANCED == status) {
            *out = id;
            --_numToReturn;
            ++_commonStats.advanced;
            return PlanStage::ADVANCED;
        }
        else if (PlanStage::FAILURE == status || PlanStage::DEAD == status) {
            *out = id;
            // If a stage fails, it may create a status WSM to indicate why it
            // failed, in which case 'id' is valid.  If ID is invalid, we
            // create our own error message.
            if (WorkingSet::INVALID_ID == id) {
                mongoutils::str::stream ss;
                ss << "limit stage failed to read in results from child";
                Status status(ErrorCodes::InternalError, ss);
                *out = WorkingSetCommon::allocateStatusMember( _ws, status);
            }
            return status;
        }
        else if (PlanStage::NEED_TIME == status) {
            ++_commonStats.needTime;
        }
        else if (PlanStage::NEED_YIELD == status) {
            ++_commonStats.needYield;
            *out = id;
        }

        return status;
    }

    void LimitStage::saveState() {
        ++_commonStats.yields;
        _child->saveState();
    }

    void LimitStage::restoreState(OperationContext* opCtx) {
        ++_commonStats.unyields;
        _child->restoreState(opCtx);
    }

    void LimitStage::invalidate(OperationContext* txn, const RecordId& dl, InvalidationType type) {
        ++_commonStats.invalidates;
        _child->invalidate(txn, dl, type);
    }

    vector<PlanStage*> LimitStage::getChildren() const {
        vector<PlanStage*> children;
        children.push_back(_child.get());
        return children;
    }

    PlanStageStats* LimitStage::getStats() {
        _commonStats.isEOF = isEOF();
        unique_ptr<PlanStageStats> ret(new PlanStageStats(_commonStats, STAGE_LIMIT));
        ret->specific.reset(new LimitStats(_specificStats));
        ret->children.push_back(_child->getStats());
        return ret.release();
    }

    const CommonStats* LimitStage::getCommonStats() const {
        return &_commonStats;
    }

    const SpecificStats* LimitStage::getSpecificStats() const {
        return &_specificStats;
    }

}  // namespace mongo
