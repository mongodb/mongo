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
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/matcher/expression.h"

namespace mongo {

    /**
     * This stage turns a DiskLoc into a BSONObj.
     *
     * In WorkingSetMember terms, it transitions from LOC_AND_IDX to LOC_AND_UNOWNED_OBJ by reading
     * the record at the provided loc.  Returns verbatim any data that already has an object.
     *
     * Preconditions: Valid DiskLoc.
     */
    class FetchStage : public PlanStage {
    public:
        FetchStage(WorkingSet* ws, PlanStage* child, const MatchExpression* filter);
        virtual ~FetchStage();

        virtual bool isEOF();
        virtual StageState work(WorkingSetID* out);

        virtual void prepareToYield();
        virtual void recoverFromYield();
        virtual void invalidate(const DiskLoc& dl);

        PlanStageStats* getStats();

    private:
        /**
         * If the member (with id memberID) passes our filter, set *out to memberID and return that
         * ADVANCED.  Otherwise, free memberID and return NEED_TIME.
         */
        StageState returnIfMatches(WorkingSetMember* member, WorkingSetID memberID,
                                   WorkingSetID* out);

        /**
         * work(...) delegates to this when we're called after requesting a fetch.
         */
        StageState fetchCompleted(WorkingSetID* out);

        // _ws is not owned by us.
        WorkingSet* _ws;
        scoped_ptr<PlanStage> _child;

        // The filter is not owned by us.
        const MatchExpression* _filter;

        // If we're fetching a DiskLoc and it points at something that's not in memory, we return a
        // a "please page this in" result and hold on to the WSID until the next call to work(...).
        WorkingSetID _idBeingPagedIn;

        // Stats
        CommonStats _commonStats;
        FetchStats _specificStats;
    };

}  // namespace mongo
