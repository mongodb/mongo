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
        auto_ptr< Cursor > newCursor() const;
        BSONObj indexKey() const;
        const char *ns() const { return fbs_.ns(); }
        BSONObj query() const { return fbs_.query(); }
        const FieldBound &bound( const char *fieldName ) const { return fbs_.bound( fieldName ); }
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

//    class QueryAborter {
//    public:
//        QueryAborter( const bool &firstDone ) :
//        firstDone_( firstDone ){}
//        class AbortException : public std::exception {
//        };
//        void mayAbort() const {
//            if ( firstDone_ )
//                throw AbortException();
//        }
//    private:
//        const bool &firstDone_;
//    };
    
    // Inherit from this interface to implement a new query operation.
    class QueryOp {
    public:
        QueryOp() : complete_(), qp_() {}
        virtual ~QueryOp() {}
        virtual void init() = 0;
        virtual void next() = 0;
        void setQueryPlan( const QueryPlan *qp ) { qp_ = qp; }
        // Return a copy of the inheriting class, which will be run with its own
        // query plan.
        virtual QueryOp *clone() const = 0;
        bool complete() const { return complete_; }
        string exceptionMessage() const { return exceptionMessage_; }
        // To be called by QueryPlanSet::Runner only.
        void setExceptionMessage( const string &exceptionMessage ) { exceptionMessage_ = exceptionMessage; }
    protected:
        void setComplete() { complete_ = true; }
        const QueryPlan &qp() { return *qp_; }
    private:
        bool complete_;
        string exceptionMessage_;
        const QueryPlan *qp_;
    };
    
    class QueryPlanSet {
    public:
        QueryPlanSet( const char *ns, const BSONObj &query, const BSONObj &order, const BSONElement *hint = 0 );
        int nPlans() const { return plans_.size(); }
        shared_ptr< QueryOp > runOp( QueryOp &op );
        template< class T >
        shared_ptr< T > runOp( T &op ) {
            return dynamic_pointer_cast< T >( runOp( static_cast< QueryOp& >( op ) ) );
        }
    private:
        struct Runner {
            Runner( QueryPlanSet &plans, QueryOp &op );
            shared_ptr< QueryOp > run();
            QueryOp &op_;
            QueryPlanSet &plans_;
//            boost::barrier startBarrier_;
//            bool firstDone_;            
        };
//        struct RunnerSet {
//            RunnerSet( QueryPlanSet &plans, QueryOp &op );
//            shared_ptr< QueryOp > run();
//            QueryOp &op_;
//            QueryPlanSet &plans_;
//            boost::barrier startBarrier_;
//            bool firstDone_;            
//        };
//        struct Runner {
//            Runner( QueryPlan &plan, RunnerSet &set, QueryOp &op ) :
//            plan_( plan ),
//            set_( set ),
//            op_( op ) {}
//            void operator()() {
//                try {
//                    set_.startBarrier_.wait();
//                    QueryAborter aborter( set_.firstDone_ );
//                    op_.run( plan_, aborter );
//                    set_.firstDone_ = true;
//                    op_.setComplete();
//                } catch ( const QueryAborter::AbortException & ) {
//                } catch ( const std::exception &e ) {
//                    exceptionMessage_ = e.what();
//                } catch ( ... ) {
//                    exceptionMessage_ = "Caught unknown exception";
//                }
//            }
//            QueryPlan &plan_;
//            RunnerSet &set_;
//            QueryOp &op_;
//            string exceptionMessage_;
//        };
        FieldBoundSet fbs_;
        typedef boost::shared_ptr< QueryPlan > PlanPtr;
        typedef vector< PlanPtr > PlanSet;
        PlanSet plans_;
    };
    
//    class QueryOptimizer {
//    public:
//        static QueryPlan getPlan(
//            const char *ns,
//            BSONObj* query,
//            BSONObj* order = 0,
//            BSONObj* hint = 0);
//    };

} // namespace mongo
