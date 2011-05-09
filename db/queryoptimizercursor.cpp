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

namespace mongo {
    
    class QueryOptimizerCursorOp : public QueryOp {
    public:
        QueryOptimizerCursorOp() : _matchCount(), _nscanned() {}
        
        virtual void _init() {
            _c = qp().newCursor();
        }
        
        virtual long long nscanned() {
            return _c.get() ? _c->nscanned() : _nscanned;
        }
        
        virtual bool prepareToYield() {
            return false;
        }
        
        virtual void recoverFromYield() {
        }
        
        virtual void next() {
            if ( _matchCount >= 101 ) {
                // This is equivalent to the default condition for switching from
                // a query to a getMore.
                _currLoc.Null();
                _currKey = BSONObj();
                setStop();
                return;
            }
            if ( ! _c || !_c->ok() ) {
                _currLoc.Null();
                _currKey = BSONObj();
                setComplete();
                return;
            }
            
            _nscanned = _c->nscanned();
            if ( matcher()->matchesCurrent( _c.get() ) && !_c->getsetdup( _c->currLoc() ) ) {
                ++_matchCount;
            }
            _currLoc = _c->currLoc();
            _currKey = _c->currKey();
            _c->advance();
        }
        virtual QueryOp *_createChild() const {
            QueryOptimizerCursorOp *ret = new QueryOptimizerCursorOp();
            ret->_matchCount = _matchCount;
            return ret;
        }
        DiskLoc currLoc() const { return _currLoc; }
        BSONObj currKey() const { return _currKey; }
        virtual bool mayRecordPlan() const {
            return complete() && !stopRequested();
        }
        shared_ptr<Cursor> cursor() const { return _c; }
    private:
        int _matchCount;
        BSONObj _currKey;
        DiskLoc _currLoc;        
        long long _nscanned;
        shared_ptr<Cursor> _c;
    };
    
    class QueryOptimizerCursor : public Cursor {
    public:
        QueryOptimizerCursor( const char *ns, const BSONObj &query ):
        _mps( new MultiPlanScanner( ns, query, BSONObj() ) ),
        _originalOp(),
        _currOp() {
            shared_ptr<QueryOp> op = _mps->nextOp( _originalOp );
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
            
            if ( !ok() ) {
                return false;
            }
            
            shared_ptr<QueryOp> op = _mps->nextOp( _originalOp );
            QueryOptimizerCursorOp *qocop = dynamic_cast<QueryOptimizerCursorOp*>( op.get() );

            _currOp = 0;
            if ( !op->complete() ) {
                // 'qocop' will be valid until we call _mps->nextOp() again.
                _currOp = qocop;
            }
            else if ( op->stopRequested() ) {
                _takeover.reset( new MultiCursor( _mps, qocop->cursor(), op->matcher(), *op ) );
            }
            
            return ok();
        }
        virtual BSONObj currKey() const {
            assertOk();
            return _takeover ? _takeover->currKey() : _currOp->currKey();
        }
        
        virtual DiskLoc refLoc() { return DiskLoc(); }
        
        virtual bool supportGetMore() { return false; }
        virtual bool supportYields() { return false; }
        
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
        
        virtual long long nscanned() { return -1; }

        virtual CoveredIndexMatcher *matcher() const {
            assertOk();
            return _takeover ? _takeover->matcher() : _currOp->matcher().get();
        }
        
    private:
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
        QueryOptimizerCursorOp _originalOp;
        QueryOptimizerCursorOp *_currOp;
        set<DiskLoc> _dups;
        shared_ptr<Cursor> _takeover;
    };
    
    shared_ptr<Cursor> newQueryOptimizerCursor( const char *ns, const BSONObj &query ) {
     	return shared_ptr<Cursor>( new QueryOptimizerCursor( ns, query ) );
    }
    
} // namespace mongo;
