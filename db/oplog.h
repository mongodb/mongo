// oplog.h - writing to and reading from oplog

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

/* 

     local.oplog.$main is the default
*/

#pragma once

#include "pdfile.h"
#include "db.h"
#include "dbhelpers.h"
#include "query.h"
#include "queryoptimizer.h"
#include "../client/dbclient.h"
#include "../util/optime.h"
#include "../util/timer.h"

namespace mongo {

    void createOplog();

    void _logOpObjRS(const BSONObj& op);

    /** Write operation to the log (local.oplog.$main)
      
       @param opstr
        "i" insert
        "u" update
        "d" delete
        "c" db cmd
        "n" no-op
        "db" declares presence of a database (ns is set to the db name + '.')

       See _logOp() in oplog.cpp for more details.   
    */
    void logOp(const char *opstr, const char *ns, const BSONObj& obj, BSONObj *patt = 0, bool *b = 0);

    void logKeepalive();

    /** puts obj in the oplog as a comment (a no-op).  Just for diags. 
        convention is 
          { msg : "text", ... }
    */
    void logOpComment(const BSONObj& obj);

    void oplogCheckCloseDatabase( Database * db );
    
    extern int __findingStartInitialTimeout; // configurable for testing    

    class FindingStartCursor {
    public:
        FindingStartCursor( const QueryPlan & qp ) : 
        _qp( qp ),
        _findingStart( true ),
        _findingStartMode(),
        _findingStartTimer( 0 ),
        _findingStartCursor( 0 )
        { init(); }
        bool done() const { return !_findingStart; }
        shared_ptr<Cursor> cRelease() { return _c; }
        void next() {
            if ( !_findingStartCursor || !_findingStartCursor->c->ok() ) {
                _findingStart = false;
                _c = _qp.newCursor(); // on error, start from beginning
                destroyClientCursor();
                return;
            }
            switch( _findingStartMode ) {
                case Initial: {
                    if ( !_matcher->matches( _findingStartCursor->c->currKey(), _findingStartCursor->c->currLoc() ) ) {
                        _findingStart = false; // found first record out of query range, so scan normally
                        _c = _qp.newCursor( _findingStartCursor->c->currLoc() );
                        destroyClientCursor();
                        return;
                    }
                    _findingStartCursor->c->advance();
                    RARELY {
                        if ( _findingStartTimer.seconds() >= __findingStartInitialTimeout ) {
                            createClientCursor( startLoc( _findingStartCursor->c->currLoc() ) );
                            _findingStartMode = FindExtent;
                            return;
                        }
                    }
                    return;
                }
                case FindExtent: {
                    if ( !_matcher->matches( _findingStartCursor->c->currKey(), _findingStartCursor->c->currLoc() ) ) {
                        _findingStartMode = InExtent;
                        return;
                    }
                    DiskLoc prev = prevLoc( _findingStartCursor->c->currLoc() );
                    if ( prev.isNull() ) { // hit beginning, so start scanning from here
                        createClientCursor();
                        _findingStartMode = InExtent;
                        return;
                    }
                    // There might be a more efficient implementation than creating new cursor & client cursor each time,
                    // not worrying about that for now
                    createClientCursor( prev );
                    return;
                }
                case InExtent: {
                    if ( _matcher->matches( _findingStartCursor->c->currKey(), _findingStartCursor->c->currLoc() ) ) {
                        _findingStart = false; // found first record in query range, so scan normally
                        _c = _qp.newCursor( _findingStartCursor->c->currLoc() );
                        destroyClientCursor();
                        return;
                    }
                    _findingStartCursor->c->advance();
                    return;
                }
                default: {
                    massert( 12600, "invalid _findingStartMode", false );
                }
            }                
        }     
        bool prepareToYield() {
            if ( _findingStartCursor ) {
                return _findingStartCursor->prepareToYield( _yieldData );
            }
            return true;
        }
        void recoverFromYield() {
            if ( _findingStartCursor ) {
                if ( !ClientCursor::recoverFromYield( _yieldData ) ) {
                    _findingStartCursor = 0;
                }
            }
        }        
    private:
        enum FindingStartMode { Initial, FindExtent, InExtent };
        const QueryPlan &_qp;
        bool _findingStart;
        FindingStartMode _findingStartMode;
        auto_ptr< CoveredIndexMatcher > _matcher;
        Timer _findingStartTimer;
        ClientCursor * _findingStartCursor;
        shared_ptr<Cursor> _c;
        ClientCursor::YieldData _yieldData;
        DiskLoc startLoc( const DiskLoc &rec ) {
            Extent *e = rec.rec()->myExtent( rec );
            if ( !_qp.nsd()->capLooped() || ( e->myLoc != _qp.nsd()->capExtent ) )
                return e->firstRecord;
            // Likely we are on the fresh side of capExtent, so return first fresh record.
            // If we are on the stale side of capExtent, then the collection is small and it
            // doesn't matter if we start the extent scan with capFirstNewRecord.
            return _qp.nsd()->capFirstNewRecord;
        }
        
        // should never have an empty extent in the oplog, so don't worry about that case
        DiskLoc prevLoc( const DiskLoc &rec ) {
            Extent *e = rec.rec()->myExtent( rec );
            if ( _qp.nsd()->capLooped() ) {
                if ( e->xprev.isNull() )
                    e = _qp.nsd()->lastExtent.ext();
                else
                    e = e->xprev.ext();
                if ( e->myLoc != _qp.nsd()->capExtent )
                    return e->firstRecord;
            } else {
                if ( !e->xprev.isNull() ) {
                    e = e->xprev.ext();
                    return e->firstRecord;
                }
            }
            return DiskLoc(); // reached beginning of collection
        }
        void createClientCursor( const DiskLoc &startLoc = DiskLoc() ) {
            shared_ptr<Cursor> c = _qp.newCursor( startLoc );
            _findingStartCursor = new ClientCursor(QueryOption_NoCursorTimeout, c, _qp.ns());
        }
        void destroyClientCursor() {
            if ( _findingStartCursor ) {
                ClientCursor::erase( _findingStartCursor->cursorid );
                _findingStartCursor = 0;
            }
        }
        void init() {
            // Use a ClientCursor here so we can release db mutex while scanning
            // oplog (can take quite a while with large oplogs).
            shared_ptr<Cursor> c = _qp.newReverseCursor();
            _findingStartCursor = new ClientCursor(QueryOption_NoCursorTimeout, c, _qp.ns(), BSONObj());
            _findingStartTimer.reset();
            _findingStartMode = Initial;
            BSONElement tsElt = _qp.originalQuery()[ "ts" ];
            massert( 13044, "no ts field in query", !tsElt.eoo() );
            BSONObjBuilder b;
            b.append( tsElt );
            BSONObj tsQuery = b.obj();
            _matcher.reset(new CoveredIndexMatcher(tsQuery, _qp.indexKey()));
        }
    };

    void pretouchOperation(const BSONObj& op);
    void pretouchN(vector<BSONObj>&, unsigned a, unsigned b);

    void applyOperation_inlock(const BSONObj& op);
}
