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
    
    class QueryOptimizerCursorOp : public QueryOp {
    public:
        QueryOptimizerCursorOp( long long &aggregateNscanned ) : _matchCount(), _mustAdvance(), _nscanned(), _aggregateNscanned( aggregateNscanned ) {}
        
        virtual void _init() {
            if ( qp().scanAndOrderRequired() ) {
                throw MsgAssertionException( 14810, "order spec cannot be satisfied with index" );
            }
            _c = qp().newCursor();
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
                _c.reset();
                _cc.reset();
                
                if ( _capped ) {
                    msgassertedNoTrace( 13338, str::stream() << "capped cursor overrun: " << qp().ns() );
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
                ++_matchCount;
            }
            _mustAdvance = true;
        }
        virtual QueryOp *_createChild() const {
            QueryOptimizerCursorOp *ret = new QueryOptimizerCursorOp( _aggregateNscanned );
            ret->_matchCount = _matchCount;
            return ret;
        }
        DiskLoc currLoc() const { return _c ? _c->currLoc() : DiskLoc(); }
        BSONObj currKey() const { return _c ? _c->currKey() : BSONObj(); }
        virtual bool mayRecordPlan() const {
            return complete() && !stopRequested();
        }
        shared_ptr<Cursor> cursor() const { return _c; }
    private:
        void mayAdvance() {
            if ( _mustAdvance && _c ) {
                _c->advance();
                _mustAdvance = false;
            }
            _aggregateNscanned += ( _c->nscanned() - _nscanned );
            _nscanned = _c->nscanned();
        }
        int _matchCount;
        bool _mustAdvance;
        long long _nscanned;
        bool _capped;
        shared_ptr<Cursor> _c;
        ClientCursor::CleanupPointer _cc;
        DiskLoc _posBeforeYield;
        ClientCursor::YieldData _yieldData;
        long long &_aggregateNscanned;
    };
    
    class QueryOptimizerCursor : public Cursor {
    public:
        QueryOptimizerCursor( auto_ptr<MultiPlanScanner> &mps ) :
        _mps( mps ),
        _originalOp( new QueryOptimizerCursorOp( _nscanned ) ),
        _currOp(),
        _nscanned() {
            _mps->initialOp( _originalOp );
            shared_ptr<QueryOp> op = _mps->nextOp();
            rethrowOnError( op );
            if ( !op->complete() ) {
                _currOp = dynamic_cast<QueryOptimizerCursorOp*>( op.get() );
            }
        }
        
        virtual bool ok() { return !currLoc().isNull(); }
        virtual Record* _current() { assertOk(); return currLoc().rec(); }
        virtual BSONObj current() { assertOk(); return currLoc().obj(); }
        virtual DiskLoc currLoc() { return _currLoc(); }
        DiskLoc _currLoc() const {
            if ( _takeover ) {
                return _takeover->currLoc();
            }
            if ( _currOp ) {
                return _currOp->currLoc();
            }
            return DiskLoc();            
        }
        virtual bool advance() {
            if ( _takeover ) {
                return _takeover->advance();
            }
            
            // Ok to advance if currOp in an error state due to failed yield recovery.
            if ( !( _currOp && _currOp->error() ) && !ok() ) {
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
                    _takeover.reset( new MultiCursor( _mps,
                                                     qocop->cursor(),
                                                     op->matcher( qocop->cursor() ),
                                                     *op,
                                                     _nscanned - qocop->cursor()->nscanned() ) );
                }
            }
            
            return ok();
        }
        virtual BSONObj currKey() const {
            assertOk();
            return _takeover ? _takeover->currKey() : _currOp->currKey();
        }
        
        /** This cursor will be ignored for yielding by the client cursor implementation. */
        virtual DiskLoc refLoc() { return DiskLoc(); }
        
        virtual bool supportGetMore() { return false; }

        virtual bool supportYields() { return true; }
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
            }
            else if ( _currOp ) {
                _mps->recoverFromYield();
                if ( _currOp->error() ) {
                    // See if we can advance to a non error op.
                    advance();
                }
            }
        }
        
        virtual string toString() { return "QueryOptimizerCursor"; }
        
        virtual bool getsetdup(DiskLoc loc) {
            assertOk();
            if ( !_takeover ) {
                return getsetdupInternal( loc );                
            }
            if ( getdupInternal( loc ) ) {
                return true;   
            }
            return _takeover->getsetdup( loc );
        }
        
        virtual bool isMultiKey() const {
            assertOk();
            return _takeover ? _takeover->isMultiKey() : _currOp->cursor()->isMultiKey();
        }
        
        virtual bool modifiedKeys() const { return true; }
        
        virtual long long nscanned() { return _takeover ? _takeover->nscanned() : _nscanned; }

        virtual shared_ptr< CoveredIndexMatcher > matcherPtr() const {
            assertOk();
            return _takeover ? _takeover->matcherPtr() : _currOp->matcher( _currOp->cursor() );
        }

        virtual CoveredIndexMatcher* matcher() const {
            assertOk();
            return _takeover ? _takeover->matcher() : _currOp->matcher( _currOp->cursor() ).get();
        }

    private:
        void rethrowOnError( const shared_ptr< QueryOp > &op ) {
            // If all plans have erred out, assert.
            if ( op->error() ) {
                throw MsgAssertionException( op->exception() );   
            }
        }
        
        void assertOk() const {
            massert( 14809, "Invalid access for cursor that is not ok()", !_currLoc().isNull() );
        }
        
        bool getsetdupInternal(const DiskLoc &loc) {
            pair<set<DiskLoc>::iterator, bool> p = _dups.insert(loc);
            return !p.second;
        }

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
    
    shared_ptr<Cursor> newQueryOptimizerCursor( auto_ptr<MultiPlanScanner> mps ) {
        try {
            return shared_ptr<Cursor>( new QueryOptimizerCursor( mps ) );
        } catch( const AssertionException &e ) {
            // If there is an error off the bat it generally means there are no indexes
            // satisfying 'order'.  We return an empty shared_ptr in this case.
            return shared_ptr<Cursor>();
        }
        return shared_ptr<Cursor>( new QueryOptimizerCursor( mps ) );
    }
    
    shared_ptr<Cursor> newQueryOptimizerCursor( const char *ns, const BSONObj &query, const BSONObj &order ) {
        auto_ptr<MultiPlanScanner> mps( new MultiPlanScanner( ns, query, order ) ); // mayYield == false
        return newQueryOptimizerCursor( mps );
    }
    
    shared_ptr<Cursor> NamespaceDetailsTransient::getCursor( const char *ns, const BSONObj &query, const BSONObj &order ) {
        if ( query.isEmpty() && order.isEmpty() ) {
            // TODO This will not use a covered index.
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
            if ( !query.isEmpty() && !single->matcher() ) {
                shared_ptr<CoveredIndexMatcher> matcher( new CoveredIndexMatcher( query, single->indexKeyPattern() ) );
                single->setMatcher( matcher );
            }
            return single;
        }
        return newQueryOptimizerCursor( mps );
    }
    
} // namespace mongo;
