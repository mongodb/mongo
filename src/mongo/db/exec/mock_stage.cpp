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

#include "mongo/db/exec/mock_stage.h"

namespace mongo {

    MockStage::MockStage(WorkingSet* ws) : _ws(ws) { }

    PlanStage::StageState MockStage::work(WorkingSetID* out) {
        if (isEOF()) { return PlanStage::IS_EOF; }

        StageState state = _results.front();
        _results.pop();

        if (PlanStage::ADVANCED == state) {
            // We advanced.  Put the mock obj into the working set.
            WorkingSetID id = _ws->allocate();
            WorkingSetMember* member = _ws->get(id);
            *member = _members.front();
            _members.pop();
            *out = id;
        }

        return state;
    }

    bool MockStage::isEOF() { return _results.empty(); }

    void MockStage::pushBack(const PlanStage::StageState state) {
        _results.push(state);
    }

    void MockStage::pushBack(const WorkingSetMember& member) {
        _results.push(PlanStage::ADVANCED);
        _members.push(member);
    }

}  // namespace mongo
