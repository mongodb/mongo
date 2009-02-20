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

namespace mongo {

    class FieldBound {
    public:
        FieldBound( const BSONElement &e = emptyObj.firstElement() );
        FieldBound &operator&=( const FieldBound &other );
        BSONElement lower() const { return lower_; }
        BSONElement upper() const { return upper_; }
        bool equality() const { return lower_.woCompare( upper_, false ) == 0; }
        bool nontrivial() const {
            return
            minKey.firstElement().woCompare( lower_, false ) != 0 ||
            maxKey.firstElement().woCompare( upper_, false ) != 0;
        }
    private:
        BSONObj addObj( const BSONObj &o );
        string simpleRegexEnd( string regex );
        BSONElement lower_;
        BSONElement upper_;
        vector< BSONObj > objData_;
    };
    
    class FieldBoundSet {
    public:
        FieldBoundSet( const char *ns, const BSONObj &query );
        const FieldBound &bound( const char *fieldName ) const {
            map< string, FieldBound >::const_iterator f = bounds_.find( fieldName );
            if ( f == bounds_.end() )
                return trivialBound();
            return f->second;
        }
        int nBounds() const {
            int count = 0;
            for( map< string, FieldBound >::const_iterator i = bounds_.begin(); i != bounds_.end(); ++i )
                ++count;
            return count;
        }
        int nNontrivialBounds() const {
            int count = 0;
            for( map< string, FieldBound >::const_iterator i = bounds_.begin(); i != bounds_.end(); ++i )
                if ( i->second.nontrivial() )
                    ++count;
            return count;
        }
        const char *ns() const { return ns_; }
    private:
        static FieldBound *trivialBound_;
        static FieldBound &trivialBound();
        map< string, FieldBound > bounds_;
        const char *ns_;
        BSONObj query_;
    };
    
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
        int direction() const { return direction_; }
        BSONObj startKey() const { return startKey_; }
        BSONObj endKey() const { return endKey_; }
        auto_ptr< Cursor > newCursor() const;
        BSONObj indexKey() const;
        const char *ns() const { return fbs_.ns(); }
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
    };

    class QueryAborter {
    public:
        QueryAborter( bool &firstDone ) :
        firstDone_( firstDone ){}
        class AbortException : public std::exception {
        };
        void mayAbort() {
            if ( firstDone_ )
                throw AbortException();
        }
    private:
        bool &firstDone_;
    };
    
    class QueryOp {
    public:
        QueryOp() : done_() {}
        virtual ~QueryOp() {}
        virtual void run( const QueryPlan &qp, QueryAborter &qa ) = 0;
        virtual QueryOp *clone() const = 0;
        bool done() const { return done_; }
        void setDone() { done_ = true; }
    private:
        bool done_;
    };
    
    class QueryPlanSet {
    public:
        QueryPlanSet( const char *ns, const BSONObj &query, const BSONObj &order, const BSONElement *hint = 0 );
        int nPlans() const { return plans_.size(); }
        auto_ptr< QueryOp > runOp( QueryOp &op );
    private:
        struct RunnerSet {
            RunnerSet( QueryPlanSet &plans, QueryOp &op );
            auto_ptr< QueryOp > run();
            QueryOp &op_;
            QueryPlanSet &plans_;
            boost::barrier startBarrier_;
            bool firstDone_;            
        };
        struct Runner {
            Runner( QueryPlan &plan, RunnerSet &set, QueryOp &op ) :
            plan_( plan ),
            set_( set ),
            op_( op ) {}
            void operator()() {
                try {
                    set_.startBarrier_.wait();
                    QueryAborter aborter( set_.firstDone_ );
                    op_.run( plan_, aborter );
                    set_.firstDone_ = true;
                    op_.setDone();
                } catch ( const QueryAborter::AbortException & ) {
                }
            }
            QueryPlan &plan_;
            RunnerSet &set_;
            QueryOp &op_;
        };
        FieldBoundSet fbs_;
        typedef boost::shared_ptr< QueryPlan > PlanPtr;
        typedef vector< PlanPtr > PlanSet;
        PlanSet plans_;
    };
    
    int doCount( const char *ns, const BSONObj &cmd, string &err );
    
//    class QueryOptimizer {
//    public:
//        static QueryPlan getPlan(
//            const char *ns,
//            BSONObj* query,
//            BSONObj* order = 0,
//            BSONObj* hint = 0);
//    };

} // namespace mongo
