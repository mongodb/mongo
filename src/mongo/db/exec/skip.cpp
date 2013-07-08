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
        if (isEOF()) { return PlanStage::IS_EOF; }

        WorkingSetID id;
        StageState status = _child->work(&id);

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
        }
        else {
            // NEED_TIME/YIELD, ERROR, IS_EOF
            return status;
        }
    }

    void SkipStage::prepareToYield() { _child->prepareToYield(); }

    void SkipStage::recoverFromYield() { _child->recoverFromYield(); }

    void SkipStage::invalidate(const DiskLoc& dl) { _child->invalidate(dl); }

}  // namespace mongo
