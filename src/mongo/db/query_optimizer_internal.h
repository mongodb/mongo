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
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/base/disallow_copying.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/cursor.h"
#include "mongo/db/explain.h"
#include "mongo/db/matcher.h"
#include "mongo/db/query_plan.h"
#include "mongo/db/queryutil.h"
#include "mongo/util/elapsed_tracker.h"

#pragma once

namespace mongo {

    static const int OutOfOrderDocumentsAssertionCode = 14810;

    class FieldRangeSetPair;
    class QueryPlanSelectionPolicy;
    class OrRangeGenerator;
    class ParsedQuery;
    
    /**
     * Helper class for a QueryPlanRunner to cache and count matches.  One object of this type is
     * used per candidate QueryPlan (as there is one QueryPlanRunner per QueryPlan).
     *
     * Typical usage:
     * 1) resetMatch() - reset stored match value to Unkonwn.
     * 2) setMatch() - set match value to a definite true/false value.
     * 3) knowMatch() - check if setMatch() has been called.
     * 4) incMatch() - increment count if match is true.
     */
    class CachedMatchCounter {
    public:
        /**
         * @param aggregateNscanned - shared count of nscanned for this and other plans.
         * @param cumulativeCount - starting point for accumulated count over a series of plans.
         */
        CachedMatchCounter( long long& aggregateNscanned, int cumulativeCount );
        
        /** Set whether dup checking is enabled when counting. */
        void setCheckDups( bool checkDups ) { _checkDups = checkDups; }
        
        void resetMatch();

        /** @return true if the match was not previously recorded. */
        bool setMatch( bool match );
        
        bool knowMatch() const { return _match != Unknown; }

        void incMatch( const DiskLoc& loc );

        bool wouldIncMatch( const DiskLoc& loc ) const;
        
        bool enoughCumulativeMatchesToChooseAPlan() const;
        
        bool enoughMatchesToRecordPlan() const;
        
        int cumulativeCount() const { return _cumulativeCount; }

        int count() const { return _count; }
        
        /** Update local and aggregate nscanned counts. */
        void updateNscanned( long long nscanned );

        long long nscanned() const { return _nscanned; }

        long long& aggregateNscanned() const { return _aggregateNscanned; }

    private:
        bool getsetdup( const DiskLoc& loc );

        bool getdup( const DiskLoc& loc ) const;

        long long& _aggregateNscanned;
        long long _nscanned;
        int _cumulativeCount;
        int _count;
        bool _checkDups;
        enum MatchState { Unknown, False, True };
        MatchState _match;
        bool _counted;
        set<DiskLoc> _dups;
    };

    /**
     * Iterates through a QueryPlan's candidate matches, keeping track of accumulated nscanned.
     * Generally used along with runners for other QueryPlans in a QueryPlanRunnerQueue priority
     * queue.  Eg if there are three candidate QueryPlans evaluated in parallel, there are three
     * QueryPlanRunners, one checking for matches on each query.
     *
     * Typical usage:
     * 1) A new QueryPlanRunner is generated using createChild().
     * 2) A QueryPlan is assigned using setQueryPlan().
     * 3) init() is called to initialize the runner.
     * 4) next() is called repeatedly, with nscanned() checked after each call.
     * 5) In one of these calls to next(), setComplete() is called internally.
     * 6) The QueryPattern for the QueryPlan may be recorded as a winning plan.
     */
    class QueryPlanRunner {
        MONGO_DISALLOW_COPYING( QueryPlanRunner );
    public:
        /**
         * @param aggregateNscanned Shared long long counting total nscanned for runners for all
         *     cursors.
         * @param selectionPolicy Characterizes the set of QueryPlans allowed for this operation.
         *     See queryoptimizercursor.h for more information.
         * @param requireOrder Whether only ordered plans are allowed.
         * @param alwaysCountMatches Whether matches are to be counted regardless of ordering.
         * @param cumulativeCount Total count.
         */
        QueryPlanRunner( long long& aggregateNscanned,
                         const QueryPlanSelectionPolicy& selectionPolicy,
                         const bool& requireOrder,
                         bool alwaysCountMatches,
                         int cumulativeCount = 0 );

        /** @return QueryPlan assigned to this runner by the query optimizer. */
        const QueryPlan& queryPlan() const { return *_queryPlan; }
                
        /** Advance to the next potential matching document (eg using a cursor). */
        void next();

        /**
         * @return current 'nscanned' metric for this runner.  Used to compare cost to other
         * runners.
         */
        long long nscanned() const;

        /** Take any steps necessary before the db mutex is yielded. */
        void prepareToYield();

        /** Recover once the db mutex is regained. */
        void recoverFromYield();

        /** Take any steps necessary before an earlier iterate of the cursor is modified. */
        void prepareToTouchEarlierIterate();

        /** Recover after the earlier iterate is modified. */
        void recoverFromTouchingEarlierIterate();

        DiskLoc currLoc() const { return _c ? _c->currLoc() : DiskLoc(); }

        BSONObj currKey() const { return _c ? _c->currKey() : BSONObj(); }

        bool currentMatches( MatchDetails* details );
        
        /**
         * @return true iff the QueryPlan for this runner may be registered
         * as a winning plan.
         */
        bool mayRecordPlan() const;

        shared_ptr<Cursor> cursor() const { return _c; }

        /** @return true iff the implementation called setComplete() or setStop(). */
        bool complete() const { return _complete; }

        /** @return true iff the implementation called setStop(). */
        bool stopRequested() const { return _stopRequested; }

        bool completeWithoutStop() const { return complete() && !stopRequested(); }

        /** @return true iff the implementation errored out. */
        bool error() const { return _error; }

        /** @return the error information. */
        ExceptionInfo exception() const { return _exception; }
        
        /** To be called by QueryPlanSet::Runner only. */
        
        /**
         * @return a copy of the inheriting class, which will be run with its own query plan.  The
         * child runner will assume its parent runner has completed execution.
         */
        QueryPlanRunner* createChild() const;

        void setQueryPlan( const QueryPlan* queryPlan );

        /** Handle initialization after a QueryPlan has been set. */
        void init();

        void setException( const DBException& e );

        /** @return an ExplainPlanInfo object that will be updated as the query runs. */
        shared_ptr<ExplainPlanInfo> generateExplainInfo();

        shared_ptr<ExplainPlanInfo> explainInfo() const { return _explainPlanInfo; }

        const Projection::KeyOnly* keyFieldsOnly() const {
            return queryPlan().keyFieldsOnly().get();
        }
        
    private:
        /** Call if all results have been found. */
        void setComplete() { _complete = true; }

        /** Call if the scan is complete even if not all results have been found. */
        void setStop() { setComplete(); _stopRequested = true; }

        void mayAdvance();

        bool countingMatches();

        bool countMatches() const;

        /**
         * @return true if the results generated by this query plan will be loaded from the record
         *     store (not built from an index entry).
         */
        bool hasDocumentLoadingQueryPlan() const;

        void recordCursorLocation();

        void checkCursorAdvanced();

        void handleCursorAdvanced();

        void checkCursorOrdering();

        bool _complete;
        bool _stopRequested;
        ExceptionInfo _exception;
        const QueryPlan* _queryPlan;
        bool _error;
        CachedMatchCounter _matchCounter;
        bool _countingMatches;
        bool _mustAdvance;
        bool _capped;
        shared_ptr<Cursor> _c;
        ClientCursorHolder _cc;
        DiskLoc _posBeforeYield;
        ClientCursor::YieldData _yieldData;
        const QueryPlanSelectionPolicy& _selectionPolicy;
        const bool& _requireOrder; // TODO don't use a ref for this, but signal change explicitly
        shared_ptr<ExplainPlanInfo> _explainPlanInfo;
        bool _alwaysCountMatches;
    };

    /**
     * This class works if T::operator< is variant unlike a regular stl priority queue, but it's
     * very slow.  However if _vec.size() is always very small, it would be fine, maybe even faster
     * than a smart impl that does more memory allocations.
     * TODO Clean up this temporary code.
     */
    template<class T>
    class PriorityQueue {
        MONGO_DISALLOW_COPYING( PriorityQueue );
    public:
        PriorityQueue() {
            _vec.reserve(4);
        }
        int size() const { return _vec.size(); }
        bool empty() const { return _vec.empty(); }
        void push(const T& x) { 
            _vec.push_back(x);
        }
        T pop() { 
            size_t t = 0;
            for( size_t i = 1; i < _vec.size(); i++ ) {
                if( _vec[t] < _vec[i] )
                    t = i;
            }
            T ret = _vec[t];
            _vec.erase(_vec.begin()+t);
            return ret;
        }
    private:
        vector<T> _vec;
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
        QueryPlanGenerator( QueryPlanSet& qps,
                            auto_ptr<FieldRangeSetPair> originalFrsp,
                            const shared_ptr<const ParsedQuery>& parsedQuery,
                            const BSONObj& hint,
                            RecordedPlanPolicy recordedPlanPolicy,
                            const BSONObj& min,
                            const BSONObj& max,
                            bool allowSpecial );

        /** Populate the provided QueryPlanSet with an initial set of plans. */
        void addInitialPlans();

        /** Supplement a cached plan provided earlier by adding additional query plans. */
        void addFallbackPlans();

    private:

        bool addShortCircuitPlan( NamespaceDetails* d );

        bool addHintPlan( NamespaceDetails* d );

        bool addSpecialPlan( NamespaceDetails* d );

        void addStandardPlans( NamespaceDetails* d );

        bool addCachedPlan( NamespaceDetails* d );

        shared_ptr<QueryPlan> newPlan( NamespaceDetails* d,
                                       int idxNo,
                                       const BSONObj& min = BSONObj(),
                                       const BSONObj& max = BSONObj(),
                                       const string& special = "" ) const;

        bool setUnindexedPlanIf( bool set, NamespaceDetails* d );

        void setSingleUnindexedPlan( NamespaceDetails* d );

        void setHintedPlanForIndex( IndexDetails& id );

        void validateAndSetHintedPlan( const shared_ptr<QueryPlan>& plan );

        void warnOnCappedIdTableScan() const;

        QueryPlanSet& _qps;
        auto_ptr<FieldRangeSetPair> _originalFrsp;
        shared_ptr<const ParsedQuery> _parsedQuery;
        BSONObj _hint;
        RecordedPlanPolicy _recordedPlanPolicy;
        BSONObj _min;
        BSONObj _max;
        bool _allowSpecial;
    };

    /** A set of candidate query plans for a query. */
    class QueryPlanSet {
    public:
        typedef boost::shared_ptr<QueryPlan> QueryPlanPtr;
        typedef vector<QueryPlanPtr> PlanVector;

        /**
         * @param originalFrsp - original constraints for this query clause; if null, frsp will be
         * used.
         */
        static QueryPlanSet* make( const char* ns,
                                   auto_ptr<FieldRangeSetPair> frsp,
                                   auto_ptr<FieldRangeSetPair> originalFrsp,
                                   const BSONObj& originalQuery,
                                   const BSONObj& order,
                                   const shared_ptr<const ParsedQuery>& parsedQuery,
                                   const BSONObj& hint,
                                   QueryPlanGenerator::RecordedPlanPolicy recordedPlanPolicy,
                                   const BSONObj& min,
                                   const BSONObj& max,
                                   bool allowSpecial );

        /** @return number of candidate plans. */
        int nPlans() const { return _plans.size(); }

        QueryPlanPtr firstPlan() const { return _plans[ 0 ]; }
        
        /** @return true if a plan is selected based on previous success of this plan. */
        bool usingCachedPlan() const { return _usingCachedPlan; }

        /** @return true if some candidate plans may have been excluded due to plan caching. */
        bool hasPossiblyExcludedPlans() const;

        /** @return a single plan that may work well for the specified query. */
        QueryPlanPtr getBestGuess() const;

        const FieldRangeSetPair& frsp() const { return *_frsp; }

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
        void setSinglePlan( const QueryPlanPtr& plan );

        /** Configure a query plan from the plan cache. */
        void setCachedPlan( const QueryPlanPtr& plan, const CachedQueryPlan& cachedPlan );

        /** Add a candidate query plan, potentially one of many. */
        void addCandidatePlan( const QueryPlanPtr& plan );

        const PlanVector& plans() const { return _plans; }

        bool mayRecordPlan() const { return _mayRecordPlan; }

        int oldNScanned() const { return _oldNScanned; }

        void addFallbackPlans();

        void setUsingCachedPlan( bool usingCachedPlan ) { _usingCachedPlan = usingCachedPlan; }
        
        //for testing

        bool modifiedKeys() const;

        bool hasMultiKey() const;

    private:

        QueryPlanSet( const char* ns,
                      auto_ptr<FieldRangeSetPair> frsp,
                      auto_ptr<FieldRangeSetPair> originalFrsp,
                      const BSONObj& originalQuery,
                      const BSONObj& order,
                      const shared_ptr<const ParsedQuery>& parsedQuery,
                      const BSONObj& hint,
                      QueryPlanGenerator::RecordedPlanPolicy recordedPlanPolicy,
                      const BSONObj& min,
                      const BSONObj& max,
                      bool allowSpecial );

        void init();

        void pushPlan( const QueryPlanPtr& plan );

        QueryPlanGenerator _generator;
        BSONObj _originalQuery;
        auto_ptr<FieldRangeSetPair> _frsp;
        PlanVector _plans;
        bool _mayRecordPlan;
        bool _usingCachedPlan;
        CandidatePlanCharacter _cachedPlanCharacter;
        BSONObj _order;
        long long _oldNScanned;
        ElapsedTracker _yieldSometimesTracker;
        bool _allowSpecial;
    };

    /**
     * A priority queue of QueryPlanRunners ordered by their nscanned values.  The QueryPlanRunners
     * are iterated sequentially and reinserted into the queue until one runner completes or all
     * runners error out.
     */
    class QueryPlanRunnerQueue {
    public:
        QueryPlanRunnerQueue( QueryPlanSet& plans, const QueryPlanRunner& prototypeRunner );
        
        /**
         * Pull a runner from the priority queue, advance it if possible, re-insert it into the
         * queue if it is not done, and return it.  But if this runner errors out, retry with
         * another runner until a non error runner is found or all runners have errored out.
         * @return the next non error runner if there is one, otherwise an error runner.
         * If the returned runner is complete() or error(), this queue becomes done().
         */
        shared_ptr<QueryPlanRunner> next();

        /** @return true if done iterating. */
        bool done() const { return _done; }

        /** Prepare all runners for a database mutex yield. */
        void prepareToYield();

        /** Restore all runners after a database mutex yield. */
        void recoverFromYield();
        
        /** @return an ExplainClauseInfo object that will be updated as the query runs. */
        shared_ptr<ExplainClauseInfo> generateExplainInfo() {
            _explainClauseInfo.reset( new ExplainClauseInfo() );
            return _explainClauseInfo;
        }

    private:
        const QueryPlanRunner& _prototypeRunner;
        QueryPlanSet& _plans;

        static void initRunner( QueryPlanRunner& runner );

        static void nextRunner( QueryPlanRunner& runner );

        static void prepareToYieldRunner( QueryPlanRunner& runner );

        static void recoverFromYieldRunner( QueryPlanRunner& runner );
        
        /** Initialize the Runner. */
        shared_ptr<QueryPlanRunner> init();

        /** Move the Runner forward one iteration, and @return the plan for the iteration. */
        shared_ptr<QueryPlanRunner> _next();

        vector<shared_ptr<QueryPlanRunner> > _runners;
        struct RunnerHolder {
            RunnerHolder( const shared_ptr<QueryPlanRunner>& runner ) :
                _runner( runner ),
                _offset() {
            }
            shared_ptr<QueryPlanRunner> _runner;
            long long _offset;
            bool operator<( const RunnerHolder& other ) const {
                return _runner->nscanned() + _offset > other._runner->nscanned() + other._offset;
            }
        };
        PriorityQueue<RunnerHolder> _queue;
        shared_ptr<ExplainClauseInfo> _explainClauseInfo;
        bool _done;
    };

    /** Handles $or type queries by generating a QueryPlanSet for each $or clause. */
    class MultiPlanScanner {
    public:
        
        static MultiPlanScanner* make( const StringData& ns,
                                       const BSONObj& query,
                                       const BSONObj& order,
                                       const shared_ptr<const ParsedQuery>& parsedQuery =
                                               shared_ptr<const ParsedQuery>(),
                                       const BSONObj& hint = BSONObj(),
                                       QueryPlanGenerator::RecordedPlanPolicy recordedPlanPolicy =
                                               QueryPlanGenerator::Use,
                                       const BSONObj& min = BSONObj(),
                                       const BSONObj& max = BSONObj() );

        /** Set the originalRunner for QueryPlanSet iteration. */
        void initialRunner( const shared_ptr<QueryPlanRunner>& originalRunner ) {
            _baseRunner = originalRunner;
        }

        /**
         * Advance to the next runner, if not doneRunners().
         * @return the next non error runner if there is one, otherwise an error runner.
         * If the returned runner is complete() or error(), the MultiPlanScanner becomes
         * doneRunners() and no further runner iteration is possible.
         */
        shared_ptr<QueryPlanRunner> nextRunner();

        /** @return true if done with runner iteration. */
        bool doneRunners() const { return _doneRunners; }

        /**
         * Advance to the next $or clause; hasMoreClauses() must be true.
         * @param currentPlan QueryPlan of the current $or clause
         * @return best guess query plan of the next $or clause, 0 if there is no such plan.
         */
        const QueryPlan* nextClauseBestGuessPlan( const QueryPlan& currentPlan );

        /** Add explain information for a new clause. */
        void addClauseInfo( const shared_ptr<ExplainClauseInfo>& clauseInfo ) {
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
        void clearRunnerQueue();
        
        void setRecordedPlanPolicy( QueryPlanGenerator::RecordedPlanPolicy recordedPlanPolicy ) {
            _recordedPlanPolicy = recordedPlanPolicy;
        }
        
        int currentNPlans() const;

        /**
         * @return the query plan that would be used if the scanner would run a single
         * cursor for this query, otherwise 0.  The returned plan is invalid if this
         * MultiPlanScanner is destroyed, hence we return a raw pointer.
         */
        const QueryPlan* singlePlan() const;
        
        /** @return true if more $or clauses need to be scanned. */
        bool hasMoreClauses() const;

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

        MultiPlanScanner( const StringData& ns,
                          const BSONObj& query,
                          const shared_ptr<const ParsedQuery>& parsedQuery,
                          const BSONObj& hint,
                          QueryPlanGenerator::RecordedPlanPolicy recordedPlanPolicy );

        void init( const BSONObj& order,
                   const BSONObj& min,
                   const BSONObj& max );

        /** Initialize or iterate a runner generated from @param originalOp. */
        shared_ptr<QueryPlanRunner> iterateRunnerQueue( QueryPlanRunner& originalRunner,
                                                        bool retried = false );

        shared_ptr<QueryPlanRunner> nextRunnerSimple();

        shared_ptr<QueryPlanRunner> nextRunnerOr();
        
        void updateCurrentQps( QueryPlanSet* qps );
        
        void assertNotOr() const {
            massert( 13266, "not implemented for $or query", !_or );
        }

        void assertHasMoreClauses() const {
            massert( 13271, "no more clauses", hasMoreClauses() );
        }
        
        void handleEndOfClause( const QueryPlan& clausePlan );

        void handleBeginningOfClause();

        bool mayHandleBeginningOfClause();

        bool haveUselessOr() const;

        const string _ns;
        bool _or;
        BSONObj _query;
        shared_ptr<const ParsedQuery> _parsedQuery;
        scoped_ptr<OrRangeGenerator> _org; // May be null in certain non $or query cases.
        scoped_ptr<QueryPlanSet> _currentQps;
        int _i;
        QueryPlanGenerator::RecordedPlanPolicy _recordedPlanPolicy;
        BSONObj _hint;
        bool _tableScanned;
        shared_ptr<QueryPlanRunner> _baseRunner;
        scoped_ptr<QueryPlanRunnerQueue> _runnerQueue;
        shared_ptr<ExplainQueryInfo> _explainQueryInfo;
        bool _doneRunners;
    };

    /**
     * Provides a cursor interface for serial single Cursor iteration using a MultiPlanScanner.
     * Currently used internally by a QueryOptimizerCursor.
     *
     * A MultiCursor is backed by one BasicCursor or BtreeCursor at a time and forwards calls for
     * ensuring a consistent state after a write to its backing Cursor.
     */
    class MultiCursor : public Cursor {
    public:
        /** @param nscanned is the initial nscanned value. */
        MultiCursor( auto_ptr<MultiPlanScanner> mps,
                     const shared_ptr<Cursor>& c,
                     const shared_ptr<CoveredIndexMatcher>& matcher,
                     const shared_ptr<ExplainPlanInfo>& explainPlanInfo,
                     const QueryPlanRunner& runner,
                     long long nscanned );

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

        virtual bool modifiedKeys() const { return true; }

        virtual bool isMultiKey() const { return _mps->hasMultiKey(); }

        virtual CoveredIndexMatcher* matcher() const { return _matcher.get(); }

        virtual bool capped() const { return _c->capped(); }
        
        virtual long long nscanned() { return _nscanned + _c->nscanned(); }
        
        void noteIterate( bool match, bool loadedRecord );
        
        const QueryPlan& queryPlan() const {
            verify( _c->ok() && _queryPlan );
            return *_queryPlan;
        }
        
        const Projection::KeyOnly* keyFieldsOnly() const {
            verify( _c->ok() && _queryPlan );
            return _queryPlan->keyFieldsOnly().get();
        }

    private:
        void advanceClause();

        void advanceExhaustedClauses();

        auto_ptr<MultiPlanScanner> _mps;
        shared_ptr<Cursor> _c;
        shared_ptr<CoveredIndexMatcher> _matcher;
        const QueryPlan* _queryPlan;
        long long _nscanned;
        shared_ptr<ExplainPlanInfo> _explainPlanInfo;
    };

    /** NOTE min, max, and keyPattern will be updated to be consistent with the selected index. */
    IndexDetails* indexDetailsForRange( const char* ns,
                                        string& errmsg,
                                        BSONObj& min,
                                        BSONObj& max,
                                        BSONObj& keyPattern );

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
        static bool indexUseful( const FieldRangeSetPair& frsp,
                                 NamespaceDetails* d,
                                 int idxNo,
                                 const BSONObj& order );

        /** Clear any indexes recorded as the best for either the single or multi key pattern. */
        static void clearIndexesForPatterns( const FieldRangeSetPair& frsp, const BSONObj& order );

        /** Return a recorded best index for the single or multi key pattern. */
        static CachedQueryPlan bestIndexForPatterns( const FieldRangeSetPair& frsp,
                                                     const BSONObj& order );

        static bool uselessOr( const OrRangeGenerator& org, NamespaceDetails* d, int hintIdx );
    };
    
} // namespace mongo
