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
     * This stage implements skip functionality.  It drops the first 'toSkip' results from its child
     * then returns the rest verbatim.
     *
     * Preconditions: None.
     */
    class SkipStage : public PlanStage {
    public:
        SkipStage(int toSkip, WorkingSet* ws, PlanStage* child);
        virtual ~SkipStage();

        virtual bool isEOF();
        virtual StageState work(WorkingSetID* out);

        virtual void prepareToYield();
        virtual void recoverFromYield();
        virtual void invalidate(const DiskLoc& dl);

        virtual PlanStageStats* getStats();

    private:
        WorkingSet* _ws;
        scoped_ptr<PlanStage> _child;

        // We drop the first _toSkip results that we would have returned.
        int _toSkip;

        // Stats
        CommonStats _commonStats;
    };

}  // namespace mongo
