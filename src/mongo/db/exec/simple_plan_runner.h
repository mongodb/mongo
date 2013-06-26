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

#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/exec/working_set_common.h"

namespace mongo {

    /**
     * A placeholder for a full-featured plan runner.  Calls work() on a plan until a result is
     * produced.  Stops when the plan is EOF or if the plan errors.
     *
     * TODO: Yielding policy
     * TODO: Graceful error handling
     * TODO: Fetch
     * TODO: Stats, diagnostics, instrumentation, etc.
     */
    class SimplePlanRunner {
    public:
        SimplePlanRunner() { }

        WorkingSet* getWorkingSet() { return &_workingSet; }

        /**
         * Takes ownership of root.
         */
        void setRoot(PlanStage* root) {
            verify(root);
            _root.reset(root);
        }

        bool getNext(BSONObj* objOut) {
            for (;;) {
                WorkingSetID id;
                PlanStage::StageState code = _root->work(&id);

                if (PlanStage::ADVANCED == code) {
                    WorkingSetMember* member = _workingSet.get(id);
                    uassert(16912, "Couldn't fetch obj from query plan",
                            WorkingSetCommon::fetch(member));
                    *objOut = member->obj;
                    _workingSet.free(id);
                    return true;
                }
                else if (code == PlanStage::NEED_TIME) {
                    // TODO: Occasionally yield.  For now, we run until we get another result.
                }
                else {
                    // IS_EOF, ERROR, NEED_YIELD.  We just stop here.
                    return false;
                }
            }
        }

    private:
        WorkingSet _workingSet;
        scoped_ptr<PlanStage> _root;
    };

}  // namespace mongo
