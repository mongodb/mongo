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
#include "matcher.h"
#include "../util/message.h"

namespace mongo {
    
    class IndexDetails;
    class IndexType;

    class QueryPlan : boost::noncopyable {
    public:
        QueryPlan(NamespaceDetails *_d, 
                  int _idxNo, // -1 = no index
                  const FieldRangeSet &fbs,
                  const FieldRangeSet &originalFrs,
                  const BSONObj &originalQuery,
                  const BSONObj &order,
                  const BSONObj &startKey = BSONObj(),
                  const BSONObj &endKey = BSONObj() ,
                  string special="" );

        /* If true, no other index can do better. */
        bool optimal() const { return optimal_; }
        /* ScanAndOrder processing will be required if true */
        bool scanAndOrderRequired() const { return scanAndOrderRequired_; }
        /* When true, the index we are using has keys such that it can completely resolve the
         query expression to match by itself without ever checking the main object.
         */
        bool exactKeyMatch() const { return exactKeyMatch_; }
        /* If true, the startKey and endKey are unhelpful and the index order doesn't match the 
           requested sort order */
        bool unhelpful() const { return unhelpful_; }
        int direction() const { return direction_; }
        shared_ptr<Cursor> newCursor( const DiskLoc &startLoc = DiskLoc() , int numWanted=0 ) const;
        shared_ptr<Cursor> newReverseCursor() const;
        BSONObj indexKey() const;
        bool willScanTable() const { return !index_ && fbs_.matchPossible(); }
        const char *ns() const { return fbs_.ns(); }
        NamespaceDetails *nsd() const { return d; }
        BSONObj originalQuery() const { return _originalQuery; }
        BSONObj simplifiedQuery( const BSONObj& fields = BSONObj() ) const { return fbs_.simplifiedQuery( fields ); }
        const FieldRange &range( const char *fieldName ) const { return fbs_.range( fieldName ); }
        void registerSelf( long long nScanned ) const;
        shared_ptr< FieldRangeVector > originalFrv() const { return _originalFrv; }
        // just for testing
        shared_ptr< FieldRangeVector > frv() const { return _frv; }
    private:
        NamespaceDetails *d;
        int idxNo;
        const FieldRangeSet &fbs_;
        const BSONObj &_originalQuery;
        const BSONObj &order_;
        const IndexDetails *index_;
        bool optimal_;
        bool scanAndOrderRequired_;
        bool exactKeyMatch_;
        int direction_;
        shared_ptr< FieldRangeVector > _frv;
        shared_ptr< FieldRangeVector > _originalFrv;
        BSONObj _startKey;
        BSONObj _endKey;
        bool endKeyInclusive_;
        bool unhelpful_;
        string _special;
        IndexType * _type;
        bool _startOrEndSpec;
    };

    // Inherit from this interface to implement a new query operation.
    // The query optimizer will clone the QueryOp that is provided, giving
    // each clone its own query plan.
    class QueryOp {
    public:
        QueryOp() : _complete(), _stopRequested(), _qp(), _error() {}

        // Used when handing off from one QueryOp type to another
        QueryOp( const QueryOp &other ) :
        _complete(), _stopRequested(), _qp(), _error(), _matcher( other._matcher ),
        _orConstraint( other._orConstraint ) {}
        
        virtual ~QueryOp() {}
        
        /** these gets called after a query plan is set */
        void init() { 
            if ( _oldMatcher.get() ) {
                _matcher.reset( _oldMatcher->nextClauseMatcher( qp().indexKey() ) );
            } else {
                _matcher.reset( new CoveredIndexMatcher( qp().originalQuery(), qp().indexKey(), alwaysUseRecord() ) );
            }
            _init();
        }
        virtual void next() = 0;

        virtual bool mayRecordPlan() const = 0;
        
        virtual bool prepareToYield() { massert( 13335, "yield not supported", false ); return false; }
        virtual void recoverFromYield() { massert( 13336, "yield not supported", false ); }
        
        virtual long long nscanned() = 0;
        
        /** @return a copy of the inheriting class, which will be run with its own
                    query plan.  If multiple plan sets are required for an $or query,
                    the QueryOp of the winning plan from a given set will be cloned
                    to generate QueryOps for the subsequent plan set.  This function
                    should only be called after the query op has completed executing.
        */
        QueryOp *createChild() {
            if( _orConstraint.get() ) {
                _matcher->advanceOrClause( _orConstraint );
                _orConstraint.reset();
            }
            QueryOp *ret = _createChild();
            ret->_oldMatcher = _matcher;
            return ret;
        }
        bool complete() const { return _complete; }
        bool error() const { return _error; }
        bool stopRequested() const { return _stopRequested; }
        ExceptionInfo exception() const { return _exception; }
        const QueryPlan &qp() const { return *_qp; }
        // To be called by QueryPlanSet::Runner only.
        void setQueryPlan( const QueryPlan *qp ) { _qp = qp; }
        void setException( const DBException &e ) {
            _error = true;
            _exception = e.getInfo();
        }
        shared_ptr< CoveredIndexMatcher > matcher() const { return _matcher; }
    protected:
        void setComplete() {
            _orConstraint = qp().originalFrv();
            _complete = true;
        }
        void setStop() { setComplete(); _stopRequested = true; }

        virtual void _init() = 0;
        
        virtual QueryOp *_createChild() const = 0;
        
        virtual bool alwaysUseRecord() const { return false; }
    
    private:
        bool _complete;
        bool _stopRequested;
        ExceptionInfo _exception;
        const QueryPlan *_qp;
        bool _error;
        shared_ptr< CoveredIndexMatcher > _matcher;
        shared_ptr< CoveredIndexMatcher > _oldMatcher;
        shared_ptr< FieldRangeVector > _orConstraint;
    };
    
    // Set of candidate query plans for a particular query.  Used for running
    // a QueryOp on these plans.
    class QueryPlanSet {
    public:

        typedef boost::shared_ptr< QueryPlan > PlanPtr;
        typedef vector< PlanPtr > PlanSet;

        QueryPlanSet( const char *ns,
                     auto_ptr< FieldRangeSet > frs,
                     auto_ptr< FieldRangeSet > originalFrs,
                     const BSONObj &originalQuery,
                     const BSONObj &order,
                     const BSONElement *hint = 0,
                     bool honorRecordedPlan = true,
                     const BSONObj &min = BSONObj(),
                     const BSONObj &max = BSONObj(),
                     bool bestGuessOnly = false,
                     bool mayYield = false);
        int nPlans() const { return plans_.size(); }
        shared_ptr< QueryOp > runOp( QueryOp &op );
        template< class T >
        shared_ptr< T > runOp( T &op ) {
            return dynamic_pointer_cast< T >( runOp( static_cast< QueryOp& >( op ) ) );
        }
        BSONObj explain() const;
        bool usingPrerecordedPlan() const { return usingPrerecordedPlan_; }
        PlanPtr getBestGuess() const;
        //for testing
        const FieldRangeSet &fbs() const { return *fbs_; }
        const FieldRangeSet &originalFrs() const { return *_originalFrs; }
    private:
        void addOtherPlans( bool checkFirst );
        void addPlan( PlanPtr plan, bool checkFirst ) {
            if ( checkFirst && plan->indexKey().woCompare( plans_[ 0 ]->indexKey() ) == 0 )
                return;
            plans_.push_back( plan );
        }
        void init();
        void addHint( IndexDetails &id );
        struct Runner {
            Runner( QueryPlanSet &plans, QueryOp &op );
            shared_ptr< QueryOp > run();
            void mayYield( const vector< shared_ptr< QueryOp > > &ops );
            QueryOp &op_;
            QueryPlanSet &plans_;
            static void initOp( QueryOp &op );
            static void nextOp( QueryOp &op );
            static bool prepareToYield( QueryOp &op );
            static void recoverFromYield( QueryOp &op );
        };
        const char *ns;
        BSONObj _originalQuery;
        auto_ptr< FieldRangeSet > fbs_;
        auto_ptr< FieldRangeSet > _originalFrs;
        PlanSet plans_;
        bool mayRecordPlan_;
        bool usingPrerecordedPlan_;
        BSONObj hint_;
        BSONObj order_;
        long long oldNScanned_;
        bool honorRecordedPlan_;
        BSONObj min_;
        BSONObj max_;
        string _special;
        bool _bestGuessOnly;
        bool _mayYield;
        ElapsedTracker _yieldSometimesTracker;
    };

    // Handles $or type queries by generating a QueryPlanSet for each $or clause
    // NOTE on our $or implementation: In our current qo implementation we don't
    // keep statistics on our data, but we can conceptualize the problem of
    // selecting an index when statistics exist for all index ranges.  The
    // d-hitting set problem on k sets and n elements can be reduced to the
    // problem of index selection on k $or clauses and n index ranges (where
    // d is the max number of indexes, and the number of ranges n is unbounded).
    // In light of the fact that d-hitting set is np complete, and we don't even
    // track statistics (so cost calculations are expensive) our first
    // implementation uses the following greedy approach: We take one $or clause
    // at a time and treat each as a separate query for index selection purposes.
    // But if an index range is scanned for a particular $or clause, we eliminate
    // that range from all subsequent clauses.  One could imagine an opposite
    // implementation where we select indexes based on the union of index ranges
    // for all $or clauses, but this can have much poorer worst case behavior.
    // (An index range that suits one $or clause may not suit another, and this
    // is worse than the typical case of index range choice staleness because
    // with $or the clauses may likely be logically distinct.)  The greedy
    // implementation won't do any worse than all the $or clauses individually,
    // and it can often do better.  In the first cut we are intentionally using
    // QueryPattern tracking to record successful plans on $or clauses for use by
    // subsequent $or clauses, even though there may be a significant aggregate
    // $nor component that would not be represented in QueryPattern.
    class MultiPlanScanner {
    public:
        MultiPlanScanner( const char *ns,
                         const BSONObj &query,
                         const BSONObj &order,
                         const BSONElement *hint = 0,
                         bool honorRecordedPlan = true,
                         const BSONObj &min = BSONObj(),
                         const BSONObj &max = BSONObj(),
                         bool bestGuessOnly = false,
                         bool mayYield = false);
        shared_ptr< QueryOp > runOp( QueryOp &op );
        template< class T >
        shared_ptr< T > runOp( T &op ) {
            return dynamic_pointer_cast< T >( runOp( static_cast< QueryOp& >( op ) ) );
        }       
        shared_ptr< QueryOp > runOpOnce( QueryOp &op );
        template< class T >
        shared_ptr< T > runOpOnce( T &op ) {
            return dynamic_pointer_cast< T >( runOpOnce( static_cast< QueryOp& >( op ) ) );
        }       
        bool mayRunMore() const { return _or ? ( !_tableScanned && !_fros.orFinished() ) : _i == 0; }
        BSONObj oldExplain() const { assertNotOr(); return _currentQps->explain(); }
        // just report this when only one query op
        bool usingPrerecordedPlan() const {
            return !_or && _currentQps->usingPrerecordedPlan();
        }
        void setBestGuessOnly() { _bestGuessOnly = true; }
        void mayYield( bool val ) { _mayYield = val; }
    private:
        void assertNotOr() const {
            massert( 13266, "not implemented for $or query", !_or );
        }
        bool uselessOr( const BSONElement &hint ) const;
        const char * _ns;
        bool _or;
        BSONObj _query;
        FieldRangeOrSet _fros;
        auto_ptr< QueryPlanSet > _currentQps;
        int _i;
        bool _honorRecordedPlan;
        bool _bestGuessOnly;
        BSONObj _hint;
        bool _mayYield;
        bool _tableScanned;
    };
    
    class MultiCursor : public Cursor {
    public:
        class CursorOp : public QueryOp {
        public:
            CursorOp() {}
            CursorOp( const QueryOp &other ) : QueryOp( other ) {}
            virtual shared_ptr< Cursor > newCursor() const = 0;  
        };
        // takes ownership of 'op'
        MultiCursor( const char *ns, const BSONObj &pattern, const BSONObj &order, shared_ptr< CursorOp > op = shared_ptr< CursorOp >(), bool mayYield = false )
        : _mps( new MultiPlanScanner( ns, pattern, order, 0, true, BSONObj(), BSONObj(), !op.get(), mayYield ) ), _nscanned() {
            if ( op.get() ) {
                _op = op;
            } else {
                _op.reset( new NoOp() );
            }
            if ( _mps->mayRunMore() ) {
                nextClause();
                if ( !ok() ) {
                    advance();
                }
            } else {
                _c.reset( new BasicCursor( DiskLoc() ) );
            }
        }
        // used to handoff a query to a getMore()
        MultiCursor( auto_ptr< MultiPlanScanner > mps, const shared_ptr< Cursor > &c, const shared_ptr< CoveredIndexMatcher > &matcher, const QueryOp &op )
        : _op( new NoOp( op ) ), _c( c ), _mps( mps ), _matcher( matcher ), _nscanned( -1 ) {
            _mps->setBestGuessOnly();
            _mps->mayYield( false ); // with a NoOp, there's no need to yield in QueryPlanSet
            if ( !ok() ) {
                // would have been advanced by UserQueryOp if possible
                advance();
            }
        }
        virtual bool ok() { return _c->ok(); }
        virtual Record* _current() { return _c->_current(); }
        virtual BSONObj current() { return _c->current(); }
        virtual DiskLoc currLoc() { return _c->currLoc(); }
        virtual bool advance() {
            _c->advance();
            while( !ok() && _mps->mayRunMore() ) {
                nextClause();
            }
            return ok();
        }
        virtual BSONObj currKey() const { return _c->currKey(); }
        virtual DiskLoc refLoc() { return _c->refLoc(); }
        virtual void noteLocation() {
            _c->noteLocation();
        }
        virtual void checkLocation() {
            _c->checkLocation();
        }        
        virtual bool supportGetMore() { return true; }
        virtual bool supportYields() { return _c->supportYields(); }
        // with update we could potentially get the same document on multiple
        // indexes, but update appears to already handle this with seenObjects
        // so we don't have to do anything special here.
        virtual bool getsetdup(DiskLoc loc) {
            return _c->getsetdup( loc );   
        }
        virtual CoveredIndexMatcher *matcher() const { return _matcher.get(); }
        // return -1 if we're a getmore handoff
        virtual long long nscanned() { return _nscanned >= 0 ? _nscanned + _c->nscanned() : _nscanned; }
        // just for testing
        shared_ptr< Cursor > sub_c() const { return _c; }
    private:
        class NoOp : public CursorOp {
        public:
            NoOp() {}
            NoOp( const QueryOp &other ) : CursorOp( other ) {}
            virtual void _init() { setComplete(); }
            virtual void next() {}
            virtual bool mayRecordPlan() const { return false; }
            virtual QueryOp *_createChild() const { return new NoOp(); }
            virtual shared_ptr< Cursor > newCursor() const { return qp().newCursor(); }
            virtual long long nscanned() { assert( false ); return 0; }
        };
        void nextClause() {
            if ( _nscanned >= 0 && _c.get() ) {
                _nscanned += _c->nscanned();
            }
            shared_ptr< CursorOp > best = _mps->runOpOnce( *_op );
            if ( ! best->complete() )
                throw MsgAssertionException( best->exception() );
            _c = best->newCursor();
            _matcher = best->matcher();
            _op = best;
        }
        shared_ptr< CursorOp > _op;
        shared_ptr< Cursor > _c;
        auto_ptr< MultiPlanScanner > _mps;
        shared_ptr< CoveredIndexMatcher > _matcher;
        long long _nscanned;
    };
    
    // NOTE min, max, and keyPattern will be updated to be consistent with the selected index.
    IndexDetails *indexDetailsForRange( const char *ns, string &errmsg, BSONObj &min, BSONObj &max, BSONObj &keyPattern );

    inline bool isSimpleIdQuery( const BSONObj& query ){
        BSONObjIterator i(query);
        if( !i.more() ) return false;
        BSONElement e = i.next();
        if( i.more() ) return false;
        if( strcmp("_id", e.fieldName()) != 0 ) return false;
        return e.isSimpleType(); // e.g. not something like { _id : { $gt : ...
    }
    
    // matcher() will always work on the returned cursor
    inline shared_ptr< Cursor > bestGuessCursor( const char *ns, const BSONObj &query, const BSONObj &sort ) {
        if( !query.getField( "$or" ).eoo() ) {
            return shared_ptr< Cursor >( new MultiCursor( ns, query, sort ) );
        } else {
            auto_ptr< FieldRangeSet > frs( new FieldRangeSet( ns, query ) );
            auto_ptr< FieldRangeSet > origFrs( new FieldRangeSet( *frs ) );
            shared_ptr< Cursor > ret = QueryPlanSet( ns, frs, origFrs, query, sort ).getBestGuess()->newCursor();
            if ( !query.isEmpty() ) {
                shared_ptr< CoveredIndexMatcher > matcher( new CoveredIndexMatcher( query, ret->indexKeyPattern() ) );
                ret->setMatcher( matcher );
            }
            return ret;
        }
    }
        
} // namespace mongo
