/* queryoptimizer.h */

/**
*    Copyright (C) 2008 10gen Inc.
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
#include "jsobj.h"
#include "queryutil.h"

namespace mongo {
    
    class IndexDetails;
    class QueryPlan {
    public:
        QueryPlan( const FieldBoundSet &fbs, const BSONObj &order, const IndexDetails *index = 0 );
        QueryPlan( const QueryPlan &other );
        /* If true, no other index can do better. */
        bool optimal() const { return optimal_; }
        /* ScanAndOrder processing will be required if true */
        bool scanAndOrderRequired() const { return scanAndOrderRequired_; }
        /* When true, the index we are using has keys such that it can completely resolve the
         query expression to match by itself without ever checking the main object.
         */
        bool keyMatch() const { return keyMatch_; }
        /* True if keyMatch() is true, and all matches will be equal according to woEqual() */
        bool exactKeyMatch() const { return exactKeyMatch_; }
        /* If true, the startKey and endKey are unhelpful, the index order doesn't match the 
           requested sort order, and keyMatch is false */
        bool unhelpful() const { return unhelpful_; }
        int direction() const { return direction_; }
        BSONObj startKey() const { return startKey_; }
        BSONObj endKey() const { return endKey_; }
        auto_ptr< Cursor > newCursor( const DiskLoc &startLoc = DiskLoc() ) const;
        auto_ptr< Cursor > newReverseCursor() const;
        BSONObj indexKey() const;
        const char *ns() const { return fbs_.ns(); }
        BSONObj query() const { return fbs_.query(); }
        const FieldBound &bound( const char *fieldName ) const { return fbs_.bound( fieldName ); }
        void registerSelf( long long nScanned ) const;
    private:
        const FieldBoundSet &fbs_;
        const BSONObj &order_;
        const IndexDetails *index_;
        bool optimal_;
        bool scanAndOrderRequired_;
        bool keyMatch_;
        bool exactKeyMatch_;
        int direction_;
        BSONObj startKey_;
        BSONObj endKey_;
        bool unhelpful_;
    };

    // Inherit from this interface to implement a new query operation.
    class QueryOp {
    public:
        QueryOp() : complete_(), qp_(), error_() {}
        virtual ~QueryOp() {}
        virtual void init() = 0;
        virtual void next() = 0;
        virtual bool mayRecordPlan() const = 0;
        // Return a copy of the inheriting class, which will be run with its own
        // query plan.
        virtual QueryOp *clone() const = 0;
        bool complete() const { return complete_; }
        bool error() const { return error_; }
        string exceptionMessage() const { return exceptionMessage_; }
        const QueryPlan &qp() const { return *qp_; }
        // To be called by QueryPlanSet::Runner only.
        void setQueryPlan( const QueryPlan *qp ) { qp_ = qp; }
        void setExceptionMessage( const string &exceptionMessage ) {
            error_ = true;
            exceptionMessage_ = exceptionMessage;
        }
    protected:
        void setComplete() { complete_ = true; }
    private:
        bool complete_;
        string exceptionMessage_;
        const QueryPlan *qp_;
        bool error_;
    };
    
    class QueryPlanSet {
    public:
        QueryPlanSet( const char *ns, const BSONObj &query, const BSONObj &order, const BSONElement *hint = 0, bool honorRecordedPlan = true );
        int nPlans() const { return plans_.size(); }
        shared_ptr< QueryOp > runOp( QueryOp &op );
        template< class T >
        shared_ptr< T > runOp( T &op ) {
            return dynamic_pointer_cast< T >( runOp( static_cast< QueryOp& >( op ) ) );
        }
        const FieldBoundSet &fbs() const { return fbs_; }
        BSONObj explain() const;
        bool usingPrerecordedPlan() const { return usingPrerecordedPlan_; }
    private:
        void addOtherPlans( bool checkFirst );
        typedef boost::shared_ptr< QueryPlan > PlanPtr;
        typedef vector< PlanPtr > PlanSet;
        void addPlan( PlanPtr plan, bool checkFirst ) {
            if ( checkFirst && plan->indexKey().woCompare( plans_[ 0 ]->indexKey() ) == 0 )
                return;
            plans_.push_back( plan );
        }
        void init();
        struct Runner {
            Runner( QueryPlanSet &plans, QueryOp &op );
            shared_ptr< QueryOp > run();
            QueryOp &op_;
            QueryPlanSet &plans_;
            static void initOp( QueryOp &op );
            static void nextOp( QueryOp &op );
        };
        FieldBoundSet fbs_;
        PlanSet plans_;
        bool mayRecordPlan_;
        bool usingPrerecordedPlan_;
        BSONObj hint_;
        BSONObj order_;
        long long oldNScanned_;
        bool honorRecordedPlan_;
    };

} // namespace mongo
