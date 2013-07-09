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

#pragma once

#include <queue>

#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/exec/working_set.h"

namespace mongo {

    class DiskLoc;

    /**
     * MockStage is a data-producing stage that is used for testing.  Unlike the other two leaf
     * stages (CollectionScan and IndexScan) MockStage does not require any underlying storage
     * layer.
     *
     * A MockStage is "programmed" by pushing return values from work() onto its internal queue.
     * Calls to MockStage::work() pop values off that queue and return them in FIFO order,
     * annotating the working set with data when appropriate.
     */
    class MockStage : public PlanStage {
    public:
        MockStage(WorkingSet* ws);
        virtual ~MockStage() { }

        virtual StageState work(WorkingSetID* out);

        virtual bool isEOF();

        // These don't really mean anything here.
        // Some day we could count the # of calls to the yield functions to check that other stages
        // have correct yielding behavior.
        virtual void prepareToYield() { }
        virtual void recoverFromYield() { }
        virtual void invalidate(const DiskLoc& dl) { }

        /**
         * Add a result to the back of the queue.  work() goes through the queue.
         * Either no data is returned (just a state), or...
         */
        void pushBack(const PlanStage::StageState state);

        /**
         * ...data is returned (and we ADVANCED)
         */
        void pushBack(const WorkingSetMember& member);

    private:
        // We don't own this.
        WorkingSet* _ws;

        // The data we return.
        std::queue<PlanStage::StageState> _results;
        std::queue<WorkingSetMember> _members;
    };

}  // namespace mongo
