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
*/

#include "mongo/db/exec/skip.h"

namespace mongo {

    SkipStage::SkipStage(int toSkip, WorkingSet* ws, PlanStage* child)
        : _ws(ws), _child(child), _toSkip(toSkip) { }

    SkipStage::~SkipStage() { }

    bool SkipStage::isEOF() { return _child->isEOF(); }

    PlanStage::StageState SkipStage::work(WorkingSetID* out) {
        ++_commonStats.works;

        if (isEOF()) { return PlanStage::IS_EOF; }

        WorkingSetID id;
        StageState status = _child->work(&id);

        if (PlanStage::ADVANCED == status) {
            // If we're still skipping results...
            if (_toSkip > 0) {
                // ...drop the result.
                --_toSkip;
                _ws->free(id);
                ++_commonStats.needTime;
                return PlanStage::NEED_TIME;
            }

            *out = id;
            ++_commonStats.advanced;
            return PlanStage::ADVANCED;
        }
        else {
            if (PlanStage::NEED_FETCH == status) {
                ++_commonStats.needFetch;
            }
            else if (PlanStage::NEED_TIME == status) {
                ++_commonStats.needTime;
            }
            // NEED_TIME/YIELD, ERROR, IS_EOF
            return status;
        }
    }

    void SkipStage::prepareToYield() {
        ++_commonStats.yields;
        _child->prepareToYield();
    }

    void SkipStage::recoverFromYield() {
        ++_commonStats.unyields;
        _child->recoverFromYield();
    }

    void SkipStage::invalidate(const DiskLoc& dl) {
        ++_commonStats.invalidates;
        _child->invalidate(dl);
    }

    PlanStageStats* SkipStage::getStats() {
        _commonStats.isEOF = isEOF();
        auto_ptr<PlanStageStats> ret(new PlanStageStats(_commonStats));
        ret->children.push_back(_child->getStats());
        return ret.release();
    }

}  // namespace mongo
