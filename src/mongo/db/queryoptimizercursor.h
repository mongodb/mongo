/**
 *    Copyright (C) 2011 10gen Inc.
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

#include "cursor.h"

namespace mongo {
    
    class CandidatePlanCharacter;
    class ExplainQueryInfo;
    class FieldRangeSet;
    
    /**
     * Adds functionality to Cursor for running multiple plans, running out of order plans,
     * utilizing covered indexes, and generating explain output.
     */
    class QueryOptimizerCursor : public Cursor {
    public:
        
        /** Candidate plans for the query before it begins running. */
        virtual CandidatePlanCharacter initialCandidatePlans() const = 0;
        /** FieldRangeSet for the query before it begins running. */
        virtual const FieldRangeSet *initialFieldRangeSet() const = 0;

        /** @return true if the plan for the current iterate is out of order. */
        virtual bool currentPlanScanAndOrderRequired() const = 0;

        /** @return true when there may be multiple plans running and some are in order. */
        virtual bool runningInitialInOrderPlan() const = 0;
        /**
         * @return true when some query plans may have been excluded due to plan caching, for a
         * non-$or query.
         */
        virtual bool hasPossiblyExcludedPlans() const = 0;

        /**
         * @return true when both in order and out of order candidate plans were available, and
         * an out of order candidate plan completed iteration.
         */
        virtual bool completePlanOfHybridSetScanAndOrderRequired() const = 0;

        /** Clear recorded indexes for the current clause's query patterns. */
        virtual void clearIndexesForPatterns() = 0;
        /** Stop returning results from out of order plans and do not allow them to complete. */
        virtual void abortOutOfOrderPlans() = 0;

        /** Note match information for the current iterate, to generate explain output. */
        virtual void noteIterate( bool match, bool loadedDocument, bool chunkSkip ) = 0;
        /** Note a lock yield for explain output reporting. */
        virtual void noteYield() = 0;
        /** @return explain output for the query run by this cursor. */
        virtual shared_ptr<ExplainQueryInfo> explainQueryInfo() const = 0;
    };
    
} // namespace mongo
