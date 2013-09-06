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

#pragma once

#include "mongo/db/parsed_query.h"
#include "mongo/db/queryoptimizercursor.h"
#include "mongo/db/querypattern.h"

namespace mongo {

    class MultiCursor;
    class MultiPlanScanner;
    class QueryPlanRunner;
    class QueryPlanSelectionPolicy;
    struct QueryPlanSummary;
    
    /** Dup tracking class, optimizing one common case with small set and few initial reads. */
    class SmallDupSet {
    public:
        SmallDupSet() : _accesses() {
            _vec.reserve( 250 );
        }
        /** @return true if @param 'loc' already added to the set, false if adding to the set in this call. */
        bool getsetdup( const DiskLoc &loc ) {
            access();
            return vec() ? getsetdupVec( loc ) : getsetdupSet( loc );
        }
        /** @return true when @param loc in the set. */
        bool getdup( const DiskLoc &loc ) {
            access();
            return vec() ? getdupVec( loc ) : getdupSet( loc );
        }            
    private:
        void access() {
            ++_accesses;
            mayUpgrade();
        }
        void mayUpgrade() {
            if ( vec() && _accesses > 500 ) {
                _set.insert( _vec.begin(), _vec.end() );
            }
        }
        bool vec() const {
            return _set.size() == 0;
        }
        bool getsetdupVec( const DiskLoc &loc ) {
            if ( getdupVec( loc ) ) {
                return true;
            }
            _vec.push_back( loc );
            return false;
        }
        bool getdupVec( const DiskLoc &loc ) const {
            for( vector<DiskLoc>::const_iterator i = _vec.begin(); i != _vec.end(); ++i ) {
                if ( *i == loc ) {
                    return true;
                }
            }
            return false;
        }
        bool getsetdupSet( const DiskLoc &loc ) {
            pair<set<DiskLoc>::iterator, bool> p = _set.insert(loc);
            return !p.second;
        }
        bool getdupSet( const DiskLoc &loc ) {
            return _set.count( loc ) > 0;
        }
        vector<DiskLoc> _vec;
        set<DiskLoc> _set;
        long long _accesses;
    };

    /**
     * This cursor runs a MultiPlanScanner iteratively and returns results from
     * the scanner's cursors as they become available.  Once the scanner chooses
     * a single plan, this cursor becomes a simple wrapper around that single
     * plan's cursor (called the 'takeover' cursor).
     *
     * A QueryOptimizerCursor employs a delegation strategy to ensure consistency after writes
     * during its initial phase when multiple delegate Cursors may be active (before _takeover is
     * set).
     *
     * Before takeover, the return value of refLoc() will be isNull(), causing ClientCursor to
     * ignore a QueryOptimizerCursor (though not its delegate Cursors) when a delete occurs.
     * Requests to prepareToYield() or recoverFromYield() will be forwarded to
     * prepareToYield()/recoverFromYield() on ClientCursors of delegate Cursors.  If a delegate
     * Cursor becomes eof() or invalid after a yield recovery,
     * QueryOptimizerCursor::recoverFromYield() may advance _currRunner to another delegate Cursor.
     *
     * Requests to prepareToTouchEarlierIterate() or recoverFromTouchingEarlierIterate() are
     * forwarded as prepareToTouchEarlierIterate()/recoverFromTouchingEarlierIterate() to the
     * delegate Cursor when a single delegate Cursor is active.  If multiple delegate Cursors are
     * active, the advance() call preceeding prepareToTouchEarlierIterate() may not properly advance
     * all delegate Cursors, so the calls are forwarded as prepareToYield()/recoverFromYield() to a
     * ClientCursor for each delegate Cursor.
     *
     * After _takeover is set, consistency after writes is ensured by delegation to the _takeover
     * MultiCursor.
     */
    class QueryOptimizerCursorImpl : public QueryOptimizerCursor {
    public:
        static QueryOptimizerCursorImpl* make( auto_ptr<MultiPlanScanner>& mps,
                                               const QueryPlanSelectionPolicy& planPolicy,
                                               bool requireOrder,
                                               bool explain );
        
        virtual bool ok();
        
        virtual Record* _current();
        
        virtual BSONObj current();
        
        virtual DiskLoc currLoc();
        
        DiskLoc _currLoc() const;
        
        virtual bool advance();
        
        virtual BSONObj currKey() const;
        
        /**
         * When return value isNull(), our cursor will be ignored for deletions by the ClientCursor
         * implementation.  In such cases, internal ClientCursors will update the positions of
         * component Cursors when necessary.
         * !!! Use care if changing this behavior, as some ClientCursor functionality may not work
         * recursively.
         */
        virtual DiskLoc refLoc();
        
        virtual BSONObj indexKeyPattern();
        
        virtual bool supportGetMore() { return true; }

        virtual bool supportYields() { return true; }
        
        virtual void prepareToTouchEarlierIterate();

        virtual void recoverFromTouchingEarlierIterate();

        virtual void prepareToYield();
        
        virtual void recoverFromYield();
        
        virtual string toString() { return "QueryOptimizerCursor"; }
        
        virtual bool getsetdup(DiskLoc loc);
        
        /** Matcher needs to know if the the cursor being forwarded to is multikey. */
        virtual bool isMultiKey() const;
        
        // TODO fix
        virtual bool modifiedKeys() const { return true; }

        virtual bool capped() const;

        virtual long long nscanned();

        virtual CoveredIndexMatcher *matcher() const;

        virtual bool currentMatches( MatchDetails* details = 0 );
        
        virtual CandidatePlanCharacter initialCandidatePlans() const {
            return _initialCandidatePlans;
        }
        
        virtual const FieldRangeSet* initialFieldRangeSet() const;
        
        virtual bool currentPlanScanAndOrderRequired() const;
        
        virtual const Projection::KeyOnly* keyFieldsOnly() const;
        
        virtual bool runningInitialInOrderPlan() const;

        virtual bool hasPossiblyExcludedPlans() const;

        virtual bool completePlanOfHybridSetScanAndOrderRequired() const {
            return _completePlanOfHybridSetScanAndOrderRequired;
        }
        
        virtual void clearIndexesForPatterns();
        
        virtual void abortOutOfOrderPlans();

        virtual void noteIterate( bool match, bool loadedDocument, bool chunkSkip );
        
        virtual void noteYield();
        
        virtual shared_ptr<ExplainQueryInfo> explainQueryInfo() const {
            return _explainQueryInfo;
        }
        
    private:
        
        QueryOptimizerCursorImpl( auto_ptr<MultiPlanScanner>& mps,
                                  const QueryPlanSelectionPolicy& planPolicy,
                                  bool requireOrder );
        
        void init( bool explain );

        /**
         * Advances the QueryPlanSet::Runner.
         * @param force - advance even if the current query op is not valid.  The 'force' param should only be specified
         * when there are plans left in the runner.
         */
        bool _advance( bool force );

        /** Forward an exception when the runner errs out. */
        void rethrowOnError( const shared_ptr< QueryPlanRunner >& runner );
        
        void assertOk() const {
            massert( 14809, "Invalid access for cursor that is not ok()", !_currLoc().isNull() );
        }

        /** Insert and check for dups before takeover occurs */
        bool getsetdupInternal(const DiskLoc& loc);

        /** Just check for dups - after takeover occurs */
        bool getdupInternal(const DiskLoc& loc);
        
        bool _requireOrder;
        auto_ptr<MultiPlanScanner> _mps;
        CandidatePlanCharacter _initialCandidatePlans;
        shared_ptr<QueryPlanRunner> _originalRunner;
        QueryPlanRunner* _currRunner;
        bool _completePlanOfHybridSetScanAndOrderRequired;
        shared_ptr<MultiCursor> _takeover;
        long long _nscanned;
        // Using a SmallDupSet seems a bit hokey, but I've measured a 5% performance improvement
        // with ~100 document non multi key scans.
        SmallDupSet _dups;
        shared_ptr<ExplainQueryInfo> _explainQueryInfo;
    };
    
    /**
     * Helper class for generating a simple Cursor or QueryOptimizerCursor from a set of query
     * parameters.  This class was refactored from a single function call and is not expected to
     * outlive its constructor arguments.
     */
    class CursorGenerator {
    public:
        CursorGenerator( const StringData& ns,
                        const BSONObj &query,
                        const BSONObj &order,
                        const QueryPlanSelectionPolicy &planPolicy,
                        const shared_ptr<const ParsedQuery> &parsedQuery,
                        bool requireOrder,
                        QueryPlanSummary *singlePlanSummary );
        
        shared_ptr<Cursor> generate();
        
    private:
        bool snapshot() const { return _parsedQuery && _parsedQuery->isSnapshot(); }
        bool explain() const { return _parsedQuery && _parsedQuery->isExplain(); }
        BSONObj min() const { return _parsedQuery ? _parsedQuery->getMin() : BSONObj(); }
        BSONObj max() const { return _parsedQuery ? _parsedQuery->getMax() : BSONObj(); }
        bool hasFields() const { return _parsedQuery && _parsedQuery->getFieldPtr(); }
        
        bool isOrderRequired() const { return _requireOrder; }
        bool mayShortcutQueryOptimizer() const {
            return min().isEmpty() && max().isEmpty() && !hasFields() && _argumentsHint.isEmpty();
        }
        BSONObj hint() const;
        
        void setArgumentsHint();
        shared_ptr<Cursor> shortcutCursor() const;
        void setMultiPlanScanner();
        shared_ptr<Cursor> singlePlanCursor();
        
        const StringData _ns;
        BSONObj _query;
        BSONObj _order;
        const QueryPlanSelectionPolicy &_planPolicy;
        shared_ptr<const ParsedQuery> _parsedQuery;
        bool _requireOrder;
        QueryPlanSummary *_singlePlanSummary;
        
        BSONObj _argumentsHint;
        auto_ptr<MultiPlanScanner> _mps;
    };
    
} // namespace mongo
