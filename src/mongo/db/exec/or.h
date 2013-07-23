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
#include "mongo/db/matcher.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/platform/unordered_set.h"

namespace mongo {

    /**
     * This stage outputs the union of its children.  It optionally deduplicates on DiskLoc.
     *
     * Preconditions: Valid DiskLoc.
     *
     * If we're deduping, we may fail to dedup any invalidated DiskLoc properly.
     */
    class OrStage : public PlanStage {
    public:
        OrStage(WorkingSet* ws, bool dedup, Matcher* matcher);
        virtual ~OrStage();

        void addChild(PlanStage* child);

        virtual bool isEOF();

        virtual StageState work(WorkingSetID* out);

        virtual void prepareToYield();
        virtual void recoverFromYield();
        virtual void invalidate(const DiskLoc& dl);

        virtual PlanStageStats* getStats();

    private:
        // Not owned by us.
        WorkingSet* _ws;

        scoped_ptr<Matcher> _matcher;

        // Owned by us.
        vector<PlanStage*> _children;

        // Which of _children are we calling work(...) on now?
        size_t _currentChild;

        // True if we dedup on DiskLoc, false otherwise.
        bool _dedup;

        // Which DiskLocs have we returned?
        unordered_set<DiskLoc, DiskLoc::Hasher> _seen;

        // Stats
        CommonStats _commonStats;
        OrStats _specificStats;
    };

}  // namespace mongo
