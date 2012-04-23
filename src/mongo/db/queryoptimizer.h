// @file queryoptimizer.h

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
#include "queryutil.h"
#include "matcher.h"
#include "explain.h"
#include "../util/net/listen.h"
#include "mongo/db/querypattern.h"

namespace mongo {

    class IndexDetails;
    class IndexType;
    class QueryPlanSummary;
    
    /** A plan for executing a query using the given index spec and FieldRangeSet. */
    class QueryPlan : boost::noncopyable {
    public:

        /**
         * @param originalFrsp - original constraints for this query clause.  If null, frsp will be used instead.
         */
        QueryPlan(NamespaceDetails *d,
                  int idxNo, // -1 = no index
                  const FieldRangeSetPair &frsp,
                  const FieldRangeSetPair *originalFrsp,
                  const BSONObj &originalQuery,
                  const BSONObj &order,
                  const shared_ptr<const ParsedQuery> &parsedQuery =
                          shared_ptr<const ParsedQuery>(),
                  const BSONObj &startKey = BSONObj(),
                  const BSONObj &endKey = BSONObj(),
                  string special="" );

        /** @return true iff this plan cannot return any documents. */
        bool impossible() const { return _impossible; }
        /**
         * @return true iff this plan should run as the only candidate plan in the absence of an
         * impossible plan.
         */
        bool optimal() const { return _optimal; }
        /** @return true iff this plan should not be considered at all. */
        bool unhelpful() const { return _unhelpful; }
        /** @return true iff ScanAndOrder processing will be required for result set. */
        bool scanAndOrderRequired() const { return _scanAndOrderRequired; }
        /**
         * @return true iff the index we are using has keys such that it can completely resolve the
         * query expression to match by itself without ever checking the main object.
         */
        bool exactKeyMatch() const { return _exactKeyMatch; }
        /** @return true iff this QueryPlan would perform an unindexed scan. */
        bool willScanTable() const { return _idxNo < 0 && !_impossible; }
        /** @return 'special' attribute of the plan, which was either set explicitly or generated from the index. */
        const string &special() const { return _special; }
                
        /** @return a new cursor based on this QueryPlan's index and FieldRangeSet. */
        shared_ptr<Cursor> newCursor( const DiskLoc &startLoc = DiskLoc() ) const;
        /** @return a new reverse cursor if this is an unindexed plan. */
        shared_ptr<Cursor> newReverseCursor() const;
        /** Register this plan as a winner for its QueryPattern, with specified 'nscanned'. */
        void registerSelf( long long nScanned, CandidatePlanCharacter candidatePlans ) const;

        int direction() const { return _direction; }
        BSONObj indexKey() const;
        bool indexed() const { return _index != 0; }
        const IndexDetails *index() const { return _index; }
        int idxNo() const { return _idxNo; }
        const char *ns() const { return _frs.ns(); }
        NamespaceDetails *nsd() const { return _d; }
        BSONObj originalQuery() const { return _originalQuery; }
        shared_ptr<FieldRangeVector> originalFrv() const { return _originalFrv; }

        const FieldRangeSet &multikeyFrs() const { return _frsMulti; }
        
        shared_ptr<Projection::KeyOnly> keyFieldsOnly() const { return _keyFieldsOnly; }
        
        QueryPlanSummary summary() const;

        /** The following member functions are for testing, or public for testing. */
        
        shared_ptr<FieldRangeVector> frv() const { return _frv; }
        bool isMultiKey() const;
        string toString() const;
        bool queryFiniteSetOrderSuffix() const;

    private:
        void checkTableScanAllowed() const;
        int independentRangesSingleIntervalLimit() const;

        NamespaceDetails * _d;
        int _idxNo;
        const FieldRangeSet &_frs;
        const FieldRangeSet &_frsMulti;
        const BSONObj _originalQuery;
        const BSONObj _order;
        shared_ptr<const ParsedQuery> _parsedQuery;
        const IndexDetails * _index;
        bool _optimal;
        bool _scanAndOrderRequired;
        bool _exactKeyMatch;
        int _direction;
        shared_ptr<FieldRangeVector> _frv;
        shared_ptr<FieldRangeVector> _originalFrv;
        BSONObj _startKey;
        BSONObj _endKey;
        bool _endKeyInclusive;
        bool _unhelpful;
        bool _impossible;
        string _special;
        IndexType * _type;
        bool _startOrEndSpec;
        shared_ptr<Projection::KeyOnly> _keyFieldsOnly;
    };

    /**
     * A QueryPlanSummary owns its own attributes and may be shared.  Currently a QueryPlan
     * should only be owned by a QueryPlanSet.
     */
    class QueryPlanSummary {
    public:
        QueryPlanSummary() :
        _scanAndOrderRequired() {
        }
        QueryPlanSummary( const QueryPlan &queryPlan ) :
        _fieldRangeSetMulti( new FieldRangeSet( queryPlan.multikeyFrs() ) ),
        _keyFieldsOnly( queryPlan.keyFieldsOnly() ),
        _scanAndOrderRequired( queryPlan.scanAndOrderRequired() ) {
        }
        bool valid() const { return _fieldRangeSetMulti; }
        shared_ptr<FieldRangeSet> _fieldRangeSetMulti;
        shared_ptr<Projection::KeyOnly> _keyFieldsOnly;
        bool _scanAndOrderRequired;
    };

    /**
     * Inherit from this interface to implement a new query operation.
     * The query optimizer will clone the QueryOp that is provided, giving
     * each clone its own query plan.
     *
     * Normal sequence of events:
     * 1) A new QueryOp is generated using createChild().
     * 2) A QueryPlan is assigned to this QueryOp with setQueryPlan().
     * 3) _init() is called on the QueryPlan.
     * 4) next() is called repeatedly, with nscanned() checked after each call.
     * 5) In one of these calls to next(), setComplete() is called.
     * 6) The QueryPattern for the QueryPlan may be recorded as a winner.
     */
    class QueryOp {
    public:
        QueryOp() : _complete(), _stopRequested(), _qp(), _error() {}

        /** Used when handing off from one QueryOp to another. */
        QueryOp( const QueryOp &other ) :
            _complete(), _stopRequested(), _qp(), _error(), _matcher( other._matcher ),
            _orConstraint( other._orConstraint ) {}

        virtual ~QueryOp() {}

        /** @return QueryPlan assigned to this QueryOp by the query optimizer. */
        const QueryPlan &qp() const { return *_qp; }
                
        /** Advance to next potential matching document (eg using a cursor). */
        virtual void next() = 0;
        /**
         * @return current 'nscanned' metric for this QueryOp.  Used to compare
         * cost to other QueryOps.
         */
        virtual long long nscanned() = 0;
        /** Take any steps necessary before the db mutex is yielded. */
        virtual void prepareToYield() = 0;
        /** Recover once the db mutex is regained. */
        virtual void recoverFromYield() = 0;
        
        /**
         * @return true iff the QueryPlan for this QueryOp may be registered
         * as a winning plan.
         */
        virtual bool mayRecordPlan() const = 0;

        /** @return true iff the implementation called setComplete() or setStop(). */
        bool complete() const { return _complete; }
        /** @return true iff the implementation called steStop(). */
        bool stopRequested() const { return _stopRequested; }
        bool completeWithoutStop() const { return complete() && !stopRequested(); }
        /** @return true iff the implementation threw an exception. */
        bool error() const { return _error; }
        /** @return the exception thrown by implementation if one was thrown. */
        ExceptionInfo exception() const { return _exception; }
        
        /** To be called by QueryPlanSet::Runner only. */
        
        QueryOp *createChild();
        void setQueryPlan( const QueryPlan *qp ) { _qp = qp; verify( _qp != NULL ); }
        void init();        
        void setException( const DBException &e ) {
            _error = true;
            _exception = e.getInfo();
        }

        shared_ptr<CoveredIndexMatcher> matcher( const shared_ptr<Cursor>& c ) const {
           return matcher( c.get() );
        }
        shared_ptr<CoveredIndexMatcher> matcher( Cursor* c ) const {
            if( ! c ) return _matcher;
            return c->matcher() ? c->matcherPtr() : _matcher;
        }

        /** @return an ExplainPlanInfo object that will be updated as the query runs. */
        virtual shared_ptr<ExplainPlanInfo> generateExplainInfo() {
            return shared_ptr<ExplainPlanInfo>( new ExplainPlanInfo() );
        }
        
    protected:
        /** Call if all results have been found. */
        void setComplete() {
            _orConstraint = qp().originalFrv();
            _complete = true;
        }
        /** Call if the scan is complete even if not all results have been found. */
        void setStop() { setComplete(); _stopRequested = true; }

        /** Handle initialization after a QueryPlan has been set. */
        virtual void _init() = 0;

        /** @return a copy of the inheriting class, which will be run with its own query plan. */
        virtual QueryOp *_createChild() const = 0;

    private:
        bool _complete;
        bool _stopRequested;
        ExceptionInfo _exception;
        const QueryPlan *_qp;
        bool _error;
        shared_ptr<CoveredIndexMatcher> _matcher;
        shared_ptr<CoveredIndexMatcher> _oldMatcher;
        shared_ptr<FieldRangeVector> _orConstraint;
    };

    // temp.  this class works if T::operator< is variant unlike a regular stl priority queue.
    // but it's very slow.  however if v.size() is always very small, it would be fine, 
    // maybe even faster than a smart impl that does more memory allocations.
    template<class T>
    class our_priority_queue : boost::noncopyable { 
        vector<T> v;
    public:
        our_priority_queue() { 
            v.reserve(4);
        }
        int size() const { return v.size(); }
        bool empty() const { return v.empty(); }
        void push(const T & x) { 
            v.push_back(x); 
        }
        T pop() { 
            size_t t = 0;
            for( size_t i = 1; i < v.size(); i++ ) { 
                if( v[t] < v[i] )
                    t = i;
            }
            T ret = v[t];
            v.erase(v.begin()+t);
            return ret;
        }
    };

    class QueryPlanSet;

    /** Populates a provided QueryPlanSet with candidate query plans, when requested. */
    class QueryPlanGenerator {
    public:

        /** Policies for utilizing recorded plans. */
        typedef enum {
            Ignore, // Ignore the recorded plan and try all candidate plans.
            UseIfInOrder, // Use the recorded plan if it is properly ordered.
            Use // Always use the recorded plan.
        } RecordedPlanPolicy;

        /** @param qps The QueryPlanSet to which plans will be provided. */
        QueryPlanGenerator( QueryPlanSet &qps,
                           auto_ptr<FieldRangeSetPair> originalFrsp,
                           const shared_ptr<const ParsedQuery> &parsedQuery,
                           const BSONObj &hint,
                           RecordedPlanPolicy recordedPlanPolicy,
                           const BSONObj &min,
                           const BSONObj &max );
        /** Populate the provided QueryPlanSet with an initial set of plans. */
        void addInitialPlans();
        /** Supplement a cached plan provided earlier by adding additional query plans. */
        void addFallbackPlans();

    private:

        bool addShortCircuitPlan( NamespaceDetails *d );
        bool addHintPlan( NamespaceDetails *d );
        bool addSpecialPlan( NamespaceDetails *d );
        void addStandardPlans( NamespaceDetails *d );
        bool addCachedPlan( NamespaceDetails *d );
        shared_ptr<QueryPlan> newPlan( NamespaceDetails *d,
                                      int idxNo,
                                      const BSONObj &min = BSONObj(),
                                      const BSONObj &max = BSONObj(),
                                      const string &special = "" ) const;
        bool setUnindexedPlanIf( bool set, NamespaceDetails *d );
        void setSingleUnindexedPlan( NamespaceDetails *d );
        void setHintedPlan( IndexDetails &id );
        void warnOnCappedIdTableScan() const;
        QueryPlanSet &_qps;
        auto_ptr<FieldRangeSetPair> _originalFrsp;
        shared_ptr<const ParsedQuery> _parsedQuery;
        BSONObj _hint;
        RecordedPlanPolicy _recordedPlanPolicy;
        BSONObj _min;
        BSONObj _max;
    };

    /**
     * A set of candidate query plans for a query.  This class can return a best guess plan or run a
     * QueryOp on all the plans.
     */
    class QueryPlanSet {
    public:
        typedef boost::shared_ptr<QueryPlan> QueryPlanPtr;
        typedef vector<QueryPlanPtr> PlanSet;

        /**
         * @param originalFrsp - original constraints for this query clause; if null, frsp will be
         * used.
         */
        QueryPlanSet( const char *ns,
                     auto_ptr<FieldRangeSetPair> frsp,
                     auto_ptr<FieldRangeSetPair> originalFrsp,
                     const BSONObj &originalQuery,
                     const BSONObj &order,
                     const shared_ptr<const ParsedQuery> &parsedQuery =
                            shared_ptr<const ParsedQuery>(),
                     const BSONObj &hint = BSONObj(),
                     QueryPlanGenerator::RecordedPlanPolicy recordedPlanPolicy =
                            QueryPlanGenerator::Use,
                     const BSONObj &min = BSONObj(),
                     const BSONObj &max = BSONObj() );

        /** @return number of candidate plans. */
        int nPlans() const { return _plans.size(); }

        QueryPlanPtr firstPlan() const { return _plans[ 0 ]; }
        
        /** @return true if a plan is selected based on previous success of this plan. */
        bool usingCachedPlan() const { return _usingCachedPlan; }
        /** @return true if some candidate plans may have been excluded due to plan caching. */
        bool hasPossiblyExcludedPlans() const;
        /** @return a single plan that may work well for the specified query. */
        QueryPlanPtr getBestGuess() const;

        const FieldRangeSetPair &frsp() const { return *_frsp; }
        BSONObj originalQuery() const { return _originalQuery; }
        BSONObj order() const { return _order; }
        
        /** @return true if an active plan is in order. */
        bool haveInOrderPlan() const;
        /** @return true if an active or fallback plan is in order. */
        bool possibleInOrderPlan() const;
        /** @return true if an active or fallback plan is out of order. */
        bool possibleOutOfOrderPlan() const;

        CandidatePlanCharacter characterizeCandidatePlans() const;
        
        bool prepareToRetryQuery();
        
        string toString() const;
        
        /** Configure a single query plan if one has not already been provided. */
        void setSinglePlan( const QueryPlanPtr &plan );
        /** Configure a query plan from the plan cache. */
        void setCachedPlan( const QueryPlanPtr &plan, const CachedQueryPlan &cachedPlan );
        /** Add a candidate query plan, potentially one of many. */
        void addCandidatePlan( const QueryPlanPtr &plan );
        
        //for testing
        bool modifiedKeys() const;
        bool hasMultiKey() const;

        class Runner {
        public:
            Runner( QueryPlanSet &plans, QueryOp &op );
            
            /**
             * Advance the runner, if it is not done().
             * @return the next non error op if there is one, otherwise an error op.
             * If the returned op is complete() or error(), the Runner becomes done().
             */
            shared_ptr<QueryOp> next();
            /** @return true if done iterating. */
            bool done() const { return _done; }
            
            void prepareToYield();
            void recoverFromYield();
            
            /** @return an ExplainClauseInfo object that will be updated as the query runs. */
            shared_ptr<ExplainClauseInfo> generateExplainInfo() {
                _explainClauseInfo.reset( new ExplainClauseInfo() );
                return _explainClauseInfo;
            }

        private:
            QueryOp &_op;
            QueryPlanSet &_plans;
            static void initOp( QueryOp &op );
            static void nextOp( QueryOp &op );
            static void prepareToYieldOp( QueryOp &op );
            static void recoverFromYieldOp( QueryOp &op );
            
            /** Initialize the Runner. */
            shared_ptr<QueryOp> init();
            /** Move the Runner forward one iteration, and @return the plan for the iteration. */
            shared_ptr<QueryOp> _next();

            vector<shared_ptr<QueryOp> > _ops;
            struct OpHolder {
                OpHolder( const shared_ptr<QueryOp> &op ) : _op( op ), _offset() {}
                shared_ptr<QueryOp> _op;
                long long _offset;
                bool operator<( const OpHolder &other ) const {
                    return _op->nscanned() + _offset > other._op->nscanned() + other._offset;
                }
            };
            our_priority_queue<OpHolder> _queue;
            shared_ptr<ExplainClauseInfo> _explainClauseInfo;
            bool _done;
        };

    private:
        void addFallbackPlans();
        void init();

        QueryPlanGenerator _generator;
        BSONObj _originalQuery;
        auto_ptr<FieldRangeSetPair> _frsp;
        PlanSet _plans;
        bool _mayRecordPlan;
        bool _usingCachedPlan;
        CandidatePlanCharacter _cachedPlanCharacter;
        BSONObj _order;
        long long _oldNScanned;
        ElapsedTracker _yieldSometimesTracker;
    };

    /** Handles $or type queries by generating a QueryPlanSet for each $or clause. */
    class MultiPlanScanner {
    public:
        MultiPlanScanner( const char *ns,
                          const BSONObj &query,
                          const BSONObj &order,
                          const shared_ptr<const ParsedQuery> &parsedQuery =
                                  shared_ptr<const ParsedQuery>(),
                          const BSONObj &hint = BSONObj(),
                          QueryPlanGenerator::RecordedPlanPolicy recordedPlanPolicy =
                                  QueryPlanGenerator::Use,
                          const BSONObj &min = BSONObj(),
                          const BSONObj &max = BSONObj() );

        /** Set the initial QueryOp for QueryPlanSet iteration. */
        void initialOp( const shared_ptr<QueryOp> &originalOp ) { _baseOp = originalOp; }
        /**
         * Advance to the next QueryOp, if not doneOps().
         * @return the next non error op if there is one, otherwise an error op.
         * If the returned op is complete() or error(), the MultiPlanScanner becomes doneOps() and
         * no further QueryOp iteration is possible.
         */
        shared_ptr<QueryOp> nextOp();
        /** @return true if done with QueryOp iteration. */
        bool doneOps() const { return _doneOps; }

        /**
         * Advance to the next $or clause; mayRunMore() must be true.
         * @param currentPlan QueryPlan of the current $or clause
         * @return best guess query plan of the next $or clause, 0 if there is no such plan.
         */
        const QueryPlan *nextClauseBestGuessPlan( const QueryPlan &currentPlan );

        /** Add explain information for a new clause. */
        void addClauseInfo( const shared_ptr<ExplainClauseInfo> &clauseInfo ) {
            verify( _explainQueryInfo );
            _explainQueryInfo->addClauseInfo( clauseInfo );
        }
        
        /** @return an ExplainQueryInfo object that will be updated as the query runs. */
        shared_ptr<ExplainQueryInfo> generateExplainInfo() {
            _explainQueryInfo.reset( new ExplainQueryInfo() );
            return _explainQueryInfo;
        }
        
        /** Yield the runner member. */
        
        void prepareToYield();
        void recoverFromYield();
        
        /** Clear the runner member. */
        void clearRunner();
        
        void setRecordedPlanPolicy( QueryPlanGenerator::RecordedPlanPolicy recordedPlanPolicy ) {
            _recordedPlanPolicy = recordedPlanPolicy;
        }
        
        int currentNPlans() const;

        /**
         * @return the query plan that would be used if the scanner would run a single
         * cursor for this query, otherwise 0.  The returned plan is invalid if this
         * MultiPlanScanner is destroyed, hence we return a raw pointer.
         */
        const QueryPlan *singlePlan() const;
        
        /** @return true iff more $or clauses need to be scanned. */
        bool mayRunMore() const {
            return _or ? ( !_tableScanned && !_org->orRangesExhausted() ) : _i == 0;
        }
        /**
         * @return plan information if there is a cached plan for a non $or query, otherwise an
         * empty object.
         */
        BSONObj cachedPlanExplainSummary() const;
        /**
         * @return true if this is not a $or query and some candidate plans may have been excluded
         * due to plan caching.
         */
        bool hasPossiblyExcludedPlans() const {
            return !_or && _currentQps->hasPossiblyExcludedPlans();
        }
        bool hasMultiKey() const { return _currentQps->hasMultiKey(); }
        
        /** Clear recorded indexes for the current QueryPlanSet's patterns. */
        void clearIndexesForPatterns() const;

        /** @return true if an active plan of _currentQps is in order. */
        bool haveInOrderPlan() const;
        /** @return true if an active or fallback plan of _currentQps is in order. */
        bool possibleInOrderPlan() const;
        /** @return true if an active or fallback plan of _currentQps is out of order. */
        bool possibleOutOfOrderPlan() const;
        
        int i() const { return _i; }
        
        string toString() const;

    private:
        /** Initialize or iterate a runner generated from @param originalOp. */
        shared_ptr<QueryOp> iterateRunner( QueryOp &originalOp, bool retried = false );

        shared_ptr<QueryOp> nextOpSimple();
        shared_ptr<QueryOp> nextOpOr();
        
        void updateCurrentQps( QueryPlanSet *qps );
        
        void assertNotOr() const {
            massert( 13266, "not implemented for $or query", !_or );
        }
        void assertMayRunMore() const {
            massert( 13271, "can't run more ops", mayRunMore() );
        }
        
        void handleEndOfClause( const QueryPlan &clausePlan );
        void handleBeginningOfClause();

        shared_ptr<QueryOp> nextOpBeginningClause();

        bool haveUselessOr() const;

        const string _ns;
        bool _or;
        BSONObj _query;
        shared_ptr<const ParsedQuery> _parsedQuery;
        scoped_ptr<OrRangeGenerator> _org; // May be null in certain non $or query cases.
        auto_ptr<QueryPlanSet> _currentQps;
        int _i;
        QueryPlanGenerator::RecordedPlanPolicy _recordedPlanPolicy;
        BSONObj _hint;
        bool _tableScanned;
        shared_ptr<QueryOp> _baseOp;
        scoped_ptr<QueryPlanSet::Runner> _runner;
        shared_ptr<ExplainQueryInfo> _explainQueryInfo;
        bool _doneOps;
    };

    /**
     * Provides a cursor interface for serial single Cursor iteration using a MultiPlanScanner.
     * Currently used internally by a QueryOptimizerCursor.
     *
     * A MultiCursor is backed by one BasicCursor or BtreeCursor at a time and forwards calls for
     * ensuring a consistent state after a write to its backing Cursor.  There is a known issue in
     * some cases when advance() causes a switch to a new BasicCursor backing (SERVER-5198).
     */
    class MultiCursor : public Cursor {
    public:
        /** @param nscanned is the initial nscanned value. */
        MultiCursor( auto_ptr<MultiPlanScanner> mps, const shared_ptr<Cursor> &c,
                    const shared_ptr<CoveredIndexMatcher> &matcher,
                    const shared_ptr<ExplainPlanInfo> &explainPlanInfo,
                    const QueryOp &op, long long nscanned );

        virtual bool ok() { return _c->ok(); }
        virtual Record* _current() { return _c->_current(); }
        virtual BSONObj current() { return _c->current(); }
        virtual DiskLoc currLoc() { return _c->currLoc(); }
        virtual bool advance();
        virtual BSONObj currKey() const { return _c->currKey(); }
        virtual DiskLoc refLoc() { return _c->refLoc(); }
        virtual void noteLocation() { _c->noteLocation(); }
        virtual void checkLocation() { _c->checkLocation(); }
        virtual void recoverFromYield();
        virtual bool supportGetMore() { return true; }
        virtual bool supportYields() { return true; }
        virtual BSONObj indexKeyPattern() { return _c->indexKeyPattern(); }

        /** Deduping documents from a prior cursor is handled by the matcher. */
        virtual bool getsetdup(DiskLoc loc) { return _c->getsetdup( loc ); }

        virtual bool autoDedup() const { return _c->autoDedup(); }

        virtual bool modifiedKeys() const { return true; }

        virtual bool isMultiKey() const { return _mps->hasMultiKey(); }

        virtual shared_ptr< CoveredIndexMatcher > matcherPtr() const { return _matcher; }
        virtual CoveredIndexMatcher* matcher() const { return _matcher.get(); }

        virtual bool capped() const { return _c->capped(); }
        
        virtual long long nscanned() { return _nscanned + _c->nscanned(); }
        
        void noteIterate( bool match, bool loadedRecord );
        
        void noteYield();
        
        const QueryPlan &queryPlan() const {
            verify( _c->ok() && _queryPlan );
            return *_queryPlan;
        }
        
        const Projection::KeyOnly *keyFieldsOnly() const {
            verify( _c->ok() && _queryPlan );
            return _queryPlan->keyFieldsOnly().get();
        }
    private:
        void nextClause();
        auto_ptr<MultiPlanScanner> _mps;
        shared_ptr<Cursor> _c;
        shared_ptr<CoveredIndexMatcher> _matcher;
        const QueryPlan *_queryPlan;
        long long _nscanned;
        shared_ptr<ExplainPlanInfo> _explainPlanInfo;
    };

    /** NOTE min, max, and keyPattern will be updated to be consistent with the selected index. */
    IndexDetails *indexDetailsForRange( const char *ns, string &errmsg, BSONObj &min, BSONObj &max, BSONObj &keyPattern );

    class CachedQueryPlan;
    
    /**
     * Add-on functionality for queryutil classes requiring access to indexing
     * functionality not currently linked to mongos.
     * TODO Clean this up a bit, possibly with separate sharded and non sharded
     * implementations for the appropriate queryutil classes or by pulling index
     * related functionality into separate wrapper classes.
     */
    struct QueryUtilIndexed {
        /** @return true if the index may be useful according to its KeySpec. */
        static bool indexUseful( const FieldRangeSetPair &frsp, NamespaceDetails *d, int idxNo, const BSONObj &order );
        /** Clear any indexes recorded as the best for either the single or multi key pattern. */
        static void clearIndexesForPatterns( const FieldRangeSetPair &frsp, const BSONObj &order );
        /** Return a recorded best index for the single or multi key pattern. */
        static CachedQueryPlan bestIndexForPatterns( const FieldRangeSetPair &frsp, const BSONObj &order );        
        static bool uselessOr( const OrRangeGenerator& org, NamespaceDetails *d, int hintIdx );
    };
    
} // namespace mongo
