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

#include "mongo/db/diskloc.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/exec/plan_stage.h"

namespace mongo {

    /**
     * This stage implements limit functionality.  It only returns 'limit' results before EOF.
     *
     * Sort has a baked-in limit, as it can optimize the sort if it has a limit.
     *
     * Preconditions: None.
     */
    class LimitStage : public PlanStage {
    public:
        LimitStage(int limit, WorkingSet* ws, PlanStage* child);
        virtual ~LimitStage();

        virtual bool isEOF();
        virtual StageState work(WorkingSetID* out);

        virtual void prepareToYield();
        virtual void recoverFromYield();
        virtual void invalidate(const DiskLoc& dl);

        virtual PlanStageStats* getStats();

    private:
        WorkingSet* _ws;
        scoped_ptr<PlanStage> _child;

        // We only return this many results.
        int _numToReturn;

        // Stats
        CommonStats _commonStats;
    };

}  // namespace mongo
