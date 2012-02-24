// @file queryoptimizercursor.h

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
#include "diskloc.h"
#include "projection.h"

namespace mongo {
    
    class QueryPlan;
    
    /**
     * An interface for policies overriding the query optimizer's default query plan selection
     * behavior.
     */
    class QueryPlanSelectionPolicy {
    public:
        virtual ~QueryPlanSelectionPolicy() {}
        virtual string name() const = 0;
        virtual bool permitOptimalNaturalPlan() const { return true; }
        virtual bool permitOptimalIdPlan() const { return true; }
        virtual bool permitPlan( const QueryPlan &plan ) const { return true; }
        virtual BSONObj planHint( const char *ns ) const { return BSONObj(); }
        
        /** Allow any query plan selection, permitting the query optimizer's default behavior. */
        static const QueryPlanSelectionPolicy &any();

        /** Prevent unindexed collection scans. */
        static const QueryPlanSelectionPolicy &indexOnly();

        /**
         * Generally hints to use the _id plan, falling back to the $natural plan.  However, the
         * $natural plan will always be used if optimal for the query.
         */
        static const QueryPlanSelectionPolicy &idElseNatural();
        
    private:
        class Any;
        static Any __any;
        class IndexOnly;
        static IndexOnly __indexOnly;
        class IdElseNatural;
        static IdElseNatural __idElseNatural;
    };

    class QueryPlanSelectionPolicy::Any : public QueryPlanSelectionPolicy {
    public:
        virtual string name() const { return "any"; }
    };
    
    class QueryPlanSelectionPolicy::IndexOnly : public QueryPlanSelectionPolicy {
    public:
        virtual string name() const { return "indexOnly"; }
        virtual bool permitOptimalNaturalPlan() const { return false; }
        virtual bool permitPlan( const QueryPlan &plan ) const;
    };

    class QueryPlanSelectionPolicy::IdElseNatural : public QueryPlanSelectionPolicy {
    public:
        virtual string name() const { return "idElseNatural"; }
        virtual bool permitPlan( const QueryPlan &plan ) const;
        virtual BSONObj planHint( const char *ns ) const;
    };
    
    class FieldRangeSet;
    class ExplainQueryInfo;
    
    class QueryOptimizerCursor : public Cursor {
    public:
        virtual const FieldRangeSet *initialFieldRangeSet() const = 0;
        virtual bool currentPlanScanAndOrderRequired() const = 0;
        virtual bool completePlanOfHybridSetScanAndOrderRequired() const = 0;
        virtual const Projection::KeyOnly *keyFieldsOnly() const = 0;
        struct CandidatePlans {
            CandidatePlans( bool mayRunInOrderPlan, bool mayRunOutOfOrderPlan ) :
            _mayRunInOrderPlan( mayRunInOrderPlan ),
            _mayRunOutOfOrderPlan( mayRunOutOfOrderPlan ) {
            }
            CandidatePlans() :
            _mayRunInOrderPlan(),
            _mayRunOutOfOrderPlan() {
            }
            bool _mayRunInOrderPlan;
            bool _mayRunOutOfOrderPlan;
            bool valid() const { return _mayRunInOrderPlan || _mayRunOutOfOrderPlan; }
            bool hybridPlanSet() const { return _mayRunInOrderPlan && _mayRunOutOfOrderPlan; }
        };
        virtual CandidatePlans initialCandidatePlans() const = 0;
        virtual bool runningInitialInOrderPlan() const = 0;
        virtual bool runningInitialCachedPlan() const = 0;
        virtual void clearIndexesForPatterns() = 0;
        virtual void abortUnorderedPlans() = 0;
        virtual void noteIterate( bool match, bool loadedDocument, bool chunkSkip ) = 0;
        virtual shared_ptr<ExplainQueryInfo> explainQueryInfo() const = 0;
    };
    
} // namespace mongo
