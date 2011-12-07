// @file queryoptimizercursor.cpp

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

#include "pch.h"
#include "queryoptimizer.h"
#include "pdfile.h"
#include "clientcursor.h"
#include "btree.h"

namespace mongo {
    
    static const int OutOfOrderDocumentsAssertionCode = 14810;
    
    /**
     * A QueryOp implementation utilized by the QueryOptimizerCursor
     */
    class QueryOptimizerCursorOp : public QueryOp {
    public:
        /**
         * @param aggregateNscanned - shared int counting total nscanned for
         * query ops for all cursors.
         */
        QueryOptimizerCursorOp( long long &aggregateNscanned, bool requireIndex ) : _matchCount(), _myMatchCount(), _mustAdvance(), _nscanned(), _capped(), _aggregateNscanned( aggregateNscanned ), _yieldRecoveryFailed(), _requireIndex( requireIndex ) {}
        
        virtual void _init() {
            if ( qp().scanAndOrderRequired() ) {
                throw MsgAssertionException( OutOfOrderDocumentsAssertionCode, "order spec cannot be satisfied with index" );
            }
            if ( _requireIndex && strcmp( qp().indexKey().firstElementFieldName(), "$natural" ) == 0 ) {
                throw MsgAssertionException( 9011, "Not an index cursor" );                
            }
            _c = qp().newCursor();
            verify( 15933, _c->supportYields() ); // The QueryOptimizerCursor::noteLocation() implementation requires _c->prepareToYield() to work.
            _capped = _c->capped();
            mayAdvance();
        }
        
        virtual long long nscanned() {
            return _c ? _c->nscanned() : _nscanned;
        }
        
        virtual bool prepareToYield() {
            if ( _c && !_cc ) {
                _cc.reset( new ClientCursor( QueryOption_NoCursorTimeout , _c , qp().ns() ) );
            }
            if ( _cc ) {
                _posBeforeYield = currLoc();
                return _cc->prepareToYield( _yieldData );
            }
            // no active cursor - ok to yield
            return true;
        }
        
        virtual void recoverFromYield() {
            if ( _cc && !ClientCursor::recoverFromYield( _yieldData ) ) {
                _yieldRecoveryFailed = true;
                _c.reset();
                _cc.reset();
                
                if ( _capped ) {
                    msgassertedNoTrace( 13338, str::stream() << "capped cursor overrun: " << qp().ns() );
                }
                else if ( qp().mustAssertOnYieldFailure() ) {
                    msgassertedNoTrace( 15892, str::stream() << "QueryOptimizerCursorOp::recoverFromYield() failed to recover" );
                }
                else {
                    // we don't fail query since we're fine with returning partial data if collection dropped
                    // also, see SERVER-2454
                }
            }
            else {
                if ( _posBeforeYield != currLoc() ) {
                    // If the yield advanced our position, the next next() will be a no op.
                    _mustAdvance = false;
                }
            }
        }
        
        virtual void next() {
            mayAdvance();
            
            if ( _matchCount >= 101 ) {
                // This is equivalent to the default condition for switching from
                // a query to a getMore.
                setStop();
                return;
            }
            if ( !_c || !_c->ok() ) {
                setComplete();
                return;
            }
            
            if ( matcher( _c )->matchesCurrent( _c.get() ) && !_c->getsetdup( _c->currLoc() ) ) {
                ++_myMatchCount;
                ++_matchCount;
            }
            _mustAdvance = true;
        }
        virtual QueryOp *_createChild() const {
            QueryOptimizerCursorOp *ret = new QueryOptimizerCursorOp( _aggregateNscanned, _requireIndex );
            ret->_matchCount = _matchCount;
            return ret;
        }
        DiskLoc currLoc() const { return _c ? _c->currLoc() : DiskLoc(); }
        BSONObj currKey() const { return _c ? _c->currKey() : BSONObj(); }
        virtual bool mayRecordPlan() const {
            // Recording after 50 matches is a historical default (101 default limit / 2).
            return !_yieldRecoveryFailed && complete() && ( !stopRequested() || _myMatchCount > 50 );
        }
        shared_ptr<Cursor> cursor() const { return _c; }
    private:
        void mayAdvance() {
            if ( !_c ) {
                return;
            }
            if ( _mustAdvance ) {
                _c->advance();
                _mustAdvance = false;
            }
            _aggregateNscanned += ( _c->nscanned() - _nscanned );
            _nscanned = _c->nscanned();
        }
        int _matchCount; // cumulative count
        int _myMatchCount; // count for this QueryOptimizerCursorOp object
        bool _mustAdvance;
        long long _nscanned;
        bool _capped;
        shared_ptr<Cursor> _c;
        ClientCursor::CleanupPointer _cc;
        DiskLoc _posBeforeYield;
        ClientCursor::YieldData _yieldData;
        long long &_aggregateNscanned;
        bool _yieldRecoveryFailed;
        bool _requireIndex;
    };
    
    /**
     * This cursor runs a MultiPlanScanner iteratively and returns results from
     * the scanner's cursors as they become available.  Once the scanner chooses
     * a single plan, this cursor becomes a simple wrapper around that single
     * plan's cursor (called the 'takeover' cursor).
     */
    class QueryOptimizerCursor : public Cursor {
    public:
        QueryOptimizerCursor( auto_ptr<MultiPlanScanner> &mps, bool requireIndex ) :
        _mps( mps ),
        _originalOp( new QueryOptimizerCursorOp( _nscanned, requireIndex ) ),
        _currOp(),
        _nscanned() {
            _mps->initialOp( _originalOp );
            shared_ptr<QueryOp> op = _mps->nextOp();
            rethrowOnError( op );
            if ( !op->complete() ) {
                _currOp = dynamic_cast<QueryOptimizerCursorOp*>( op.get() );
            }
        }
        
        virtual bool ok() { return _takeover ? _takeover->ok() : !currLoc().isNull(); }
        
        virtual Record* _current() {
            if ( _takeover ) {
                return _takeover->_current();
            }
            assertOk();
            return currLoc().rec();
        }
        
        virtual BSONObj current() {
            if ( _takeover ) {
                return _takeover->current();
            }
            assertOk();
            return currLoc().obj();
        }
        
        virtual DiskLoc currLoc() { return _takeover ? _takeover->currLoc() : _currLoc(); }
        
        DiskLoc _currLoc() const {
            verify( 14826, !_takeover );
            if ( _currOp ) {
                return _currOp->currLoc();
            }
            return DiskLoc();
        }
        
        virtual bool advance() {
            return _advance( false );
        }
        
        virtual BSONObj currKey() const {
            if ( _takeover ) {
             	return _takeover->currKey();   
            }
            assertOk();
            return _currOp->currKey();
        }
        
        /** If !_takeover, our cursor will be ignored for yielding by the client cursor implementation. */
        virtual DiskLoc refLoc() { return _takeover ? _takeover->refLoc() : DiskLoc(); }
        
        virtual BSONObj indexKeyPattern() {
            if ( _takeover ) {
                return _takeover->indexKeyPattern();
            }
            assertOk();
            return _currOp->cursor()->indexKeyPattern();
        }
        
        virtual bool supportGetMore() { return false; }

        virtual bool supportYields() { return _takeover ? _takeover->supportYields() : true; }
        
        virtual void noteLocation() {
            if ( _takeover ) {
                _takeover->noteLocation();
            }
            else if ( _currOp ) {
                verify( 15934, _mps->prepareToYield() );
            }
        }

        virtual void checkLocation() {
            if ( _takeover ) {
                _takeover->checkLocation();
            }
            else if ( _currOp ) {
                recoverFromYield();
            }
        }

        virtual bool prepareToYield() {
            if ( _takeover ) {
                return _takeover->prepareToYield();
            }
            else if ( _currOp ) {
                return _mps->prepareToYield();
            }
            else {
                return true;
            }
        }
        
        virtual void recoverFromYield() {
            if ( _takeover ) {
                _takeover->recoverFromYield();
                return;
            }
            if ( _currOp ) {
                _mps->recoverFromYield();
                if ( _currOp->error() || !ok() ) {
                    // Advance to a non error op or a following $or clause if possible.
                    _advance( true );
                }
            }
        }
        
        virtual string toString() { return "QueryOptimizerCursor"; }
        
        virtual bool getsetdup(DiskLoc loc) {
            if ( _takeover ) {
                if ( getdupInternal( loc ) ) {
                    return true;   
                }
             	return _takeover->getsetdup( loc );   
            }
            assertOk();
            return getsetdupInternal( loc );                
        }
        
        /** Matcher needs to know if the the cursor being forwarded to is multikey. */
        virtual bool isMultiKey() const {
            if ( _takeover ) {
                return _takeover->isMultiKey();
            }
            assertOk();
            return _currOp->cursor()->isMultiKey();
        }
        
        virtual bool modifiedKeys() const { return true; }
        
        virtual long long nscanned() { return _takeover ? _takeover->nscanned() : _nscanned; }

        /** @return the matcher for the takeover cursor or current active op. */
        virtual shared_ptr<CoveredIndexMatcher> matcherPtr() const {
            if ( _takeover ) {
                return _takeover->matcherPtr();
            }
            assertOk();
            return _currOp->matcher( _currOp->cursor() );
        }

        /** @return the matcher for the takeover cursor or current active op. */
        virtual CoveredIndexMatcher* matcher() const {
            if ( _takeover ) {
                return _takeover->matcher();
            }
            assertOk();
            return _currOp->matcher( _currOp->cursor() ).get();
        }

    private:
        bool _advance( bool force ) {
            if ( _takeover ) {
                return _takeover->advance();
            }

            if ( !force && !ok() ) {
                return false;
            }

            _currOp = 0;
            shared_ptr<QueryOp> op = _mps->nextOp();
            rethrowOnError( op );

            QueryOptimizerCursorOp *qocop = dynamic_cast<QueryOptimizerCursorOp*>( op.get() );
            if ( !op->complete() ) {
                // 'qocop' will be valid until we call _mps->nextOp() again.
                _currOp = qocop;
            }
            else if ( op->stopRequested() ) {
                if ( qocop->cursor() ) {
                    _mps->clearRunner();
                    _takeover.reset( new MultiCursor( _mps,
                                                     qocop->cursor(),
                                                     op->matcher( qocop->cursor() ),
                                                     *op,
                                                     _nscanned - qocop->cursor()->nscanned() ) );
                }
            }

            return ok();
        }
        void rethrowOnError( const shared_ptr< QueryOp > &op ) {
            // If all plans have erred out, assert.
            if ( op->error() ) {
                throw MsgAssertionException( op->exception() );   
            }
        }
        
        void assertOk() const {
            massert( 14809, "Invalid access for cursor that is not ok()", !_currLoc().isNull() );
        }

        /** Insert and check for dups before takeover occurs */
        bool getsetdupInternal(const DiskLoc &loc) {
            pair<set<DiskLoc>::iterator, bool> p = _dups.insert(loc);
            return !p.second;
        }

        /** Just check for dups - after takeover occurs */
        bool getdupInternal(const DiskLoc &loc) {
            return _dups.count( loc ) > 0;
        }
        
        auto_ptr<MultiPlanScanner> _mps;
        shared_ptr<QueryOptimizerCursorOp> _originalOp;
        QueryOptimizerCursorOp *_currOp;
        set<DiskLoc> _dups;
        shared_ptr<Cursor> _takeover;
        long long _nscanned;
    };
    
    shared_ptr<Cursor> newQueryOptimizerCursor( auto_ptr<MultiPlanScanner> mps, bool requireIndex ) {
        try {
            return shared_ptr<Cursor>( new QueryOptimizerCursor( mps, requireIndex ) );
        } catch( const AssertionException &e ) {
            if ( e.getCode() == OutOfOrderDocumentsAssertionCode ) {
                // If no indexes follow the requested sort order, return an
                // empty pointer.
                return shared_ptr<Cursor>();
            }
            throw;
        }
        return shared_ptr<Cursor>();
    }
    
    shared_ptr<Cursor> NamespaceDetailsTransient::getCursor( const char *ns, const BSONObj &query,
                                                            const BSONObj &order, bool requireIndex,
                                                            bool *simpleEqualityMatch ) {
        if ( simpleEqualityMatch ) {
            *simpleEqualityMatch = false;
        }
        if ( query.isEmpty() && order.isEmpty() && !requireIndex ) {
            // TODO This will not use a covered index currently.
            return theDataFileMgr.findAll( ns );
        }
        if ( isSimpleIdQuery( query ) ) {
            Database *database = cc().database();
            assert( database );
            NamespaceDetails *d = database->namespaceIndex.details(ns);
            if ( d ) {
                int idxNo = d->findIdIndex();
                if ( idxNo >= 0 ) {
                    IndexDetails& i = d->idx( idxNo );
                    BSONObj key = i.getKeyFromQuery( query );
                    return shared_ptr<Cursor>( BtreeCursor::make( d, idxNo, i, key, key, true, 1 ) );
                }
            }
        }
        auto_ptr<MultiPlanScanner> mps( new MultiPlanScanner( ns, query, order ) ); // mayYield == false
        shared_ptr<Cursor> single = mps->singleCursor();
        if ( single ) {
            if ( !( requireIndex && dynamic_cast<BasicCursor*>( single.get() ) ) ) {
                if ( !query.isEmpty() && !single->matcher() ) {
                    shared_ptr<CoveredIndexMatcher> matcher( new CoveredIndexMatcher( query, single->indexKeyPattern() ) );
                    single->setMatcher( matcher );
                }
                if ( simpleEqualityMatch ) {
                    const QueryPlan *qp = mps->singlePlan();
                    if ( qp->exactKeyMatch() && !single->matcher()->needRecord() ) {
                        *simpleEqualityMatch = true;
                    }
                }
                return single;
            }
        }
        return newQueryOptimizerCursor( mps, requireIndex );
    }

    /** This interface just available for testing. */
    shared_ptr<Cursor> newQueryOptimizerCursor( const char *ns, const BSONObj &query, const BSONObj &order, bool requireIndex ) {
        auto_ptr<MultiPlanScanner> mps( new MultiPlanScanner( ns, query, order ) ); // mayYield == false
        return newQueryOptimizerCursor( mps, requireIndex );
    }
        
} // namespace mongo;
