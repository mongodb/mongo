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

#include "mongo/db/repl/finding_start_cursor.h"

#include "mongo/client/dbclientinterface.h"
#include "mongo/db/cursor.h"
#include "mongo/db/matcher.h"
#include "mongo/db/query_plan.h"
#include "mongo/db/queryutil.h"

namespace mongo {

    // Configurable for testing.
    int FindingStartCursor::_initialTimeout = 5;

    // -------------------------------------

    FindingStartCursor *FindingStartCursor::make( const QueryPlan &qp ) {
        auto_ptr<FindingStartCursor> ret( new FindingStartCursor( qp ) );
        ret->init();
        return ret.release();
    }

    FindingStartCursor::FindingStartCursor( const QueryPlan &qp ) :
    _qp( qp ),
    _findingStart( true ),
    _findingStartMode() {
    }
    
    void FindingStartCursor::next() {
        if ( !_findingStartCursor || !_findingStartCursor->ok() ) {
            _findingStart = false;
            _c = _qp.newCursor(); // on error, start from beginning
            destroyClientCursor();
            return;
        }
        switch( _findingStartMode ) {
            // Initial mode: scan backwards from end of collection
            case Initial: {
                if ( !_matcher->matchesCurrent( _findingStartCursor->c() ) ) {
                    _findingStart = false; // found first record out of query range, so scan normally
                    _c = _qp.newCursor( _findingStartCursor->currLoc() );
                    destroyClientCursor();
                    return;
                }
                _findingStartCursor->advance();
                RARELY {
                    if ( _findingStartTimer.seconds() >= _initialTimeout ) {
                        // If we've scanned enough, switch to find extent mode.
                        createClientCursor( extentFirstLoc( _findingStartCursor->currLoc() ) );
                        _findingStartMode = FindExtent;
                        return;
                    }
                }
                return;
            }
            // FindExtent mode: moving backwards through extents, check first
            // document of each extent.
            case FindExtent: {
                if ( !_matcher->matchesCurrent( _findingStartCursor->c() ) ) {
                    _findingStartMode = InExtent;
                    return;
                }
                DiskLoc prev = prevExtentFirstLoc( _findingStartCursor->currLoc() );
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
            // InExtent mode: once an extent is chosen, find starting doc in the extent.
            case InExtent: {
                if ( _matcher->matchesCurrent( _findingStartCursor->c() ) ) {
                    _findingStart = false; // found first record in query range, so scan normally
                    _c = _qp.newCursor( _findingStartCursor->currLoc() );
                    destroyClientCursor();
                    return;
                }
                _findingStartCursor->advance();
                return;
            }
            default: {
                massert( 14038, "invalid _findingStartMode", false );
            }
        }
    }
    
    DiskLoc FindingStartCursor::extentFirstLoc( const DiskLoc &rec ) {
        Extent *e = rec.rec()->myExtent( rec );
        if ( !_qp.nsd()->capLooped() || ( e->myLoc != _qp.nsd()->capExtent() ) )
            return e->firstRecord;
        // Likely we are on the fresh side of capExtent, so return first fresh record.
        // If we are on the stale side of capExtent, then the collection is small and it
        // doesn't matter if we start the extent scan with capFirstNewRecord.
        return _qp.nsd()->capFirstNewRecord();
    }

    DiskLoc FindingStartCursor::prevExtentFirstLoc( const DiskLoc& rec ) const {
        Extent *e = rec.rec()->myExtent( rec );
        if ( _qp.nsd()->capLooped() ) {
            while( true ) {
                // Advance e to preceding extent (looping to lastExtent if necessary).
                if ( e->xprev.isNull() ) {
                    e = _qp.nsd()->lastExtent().ext();
                }
                else {
                    e = e->xprev.ext();
                }
                if ( e->myLoc == _qp.nsd()->capExtent() ) {
                    // Reached the extent containing the oldest data in the collection.
                    return DiskLoc();
                }
                if ( !e->firstRecord.isNull() ) {
                    // Return the first record of the first non empty extent encountered.
                    return e->firstRecord;
                }
            }
        }
        else {
            while( true ) {
                if ( e->xprev.isNull() ) {
                    // Reached the beginning of the collection.
                    return DiskLoc();
                }
                e = e->xprev.ext();
                if ( !e->firstRecord.isNull() ) {
                    // Return the first record of the first non empty extent encountered.
                    return e->firstRecord;
                }
            }
        }
    }
    
    void FindingStartCursor::createClientCursor( const DiskLoc &startLoc ) {
        shared_ptr<Cursor> c = _qp.newCursor( startLoc );
        _findingStartCursor.reset( new ClientCursor(QueryOption_NoCursorTimeout, c, _qp.ns()) );
    }

    bool FindingStartCursor::firstDocMatchesOrEmpty() const {
        shared_ptr<Cursor> c = _qp.newCursor();
        return !c->ok() || _matcher->matchesCurrent( c.get() );
    }
    
    void FindingStartCursor::init() {
        BSONElement tsElt = _qp.originalQuery()[ "ts" ];
        massert( 13044, "no ts field in query", !tsElt.eoo() );
        BSONObjBuilder b;
        b.append( tsElt );
        BSONObj tsQuery = b.obj();
        _matcher.reset(new CoveredIndexMatcher(tsQuery, _qp.indexKey()));
        if ( firstDocMatchesOrEmpty() ) {
            _c = _qp.newCursor();
            _findingStart = false;
            return;
        }
        // Use a ClientCursor here so we can release db mutex while scanning
        // oplog (can take quite a while with large oplogs).
        shared_ptr<Cursor> c = _qp.newReverseCursor();
        _findingStartCursor.reset( new ClientCursor(QueryOption_NoCursorTimeout, c, _qp.ns(), BSONObj()) );
        _findingStartTimer.reset();
        _findingStartMode = Initial;
    }
    
    shared_ptr<Cursor> FindingStartCursor::getCursor( const char *ns, const BSONObj &query, const BSONObj &order ) {
        NamespaceDetails *d = nsdetails(ns);
        if ( !d ) {
            return shared_ptr<Cursor>( new BasicCursor( DiskLoc() ) );
        }
        FieldRangeSetPair frsp( ns, query );
        scoped_ptr<QueryPlan> oplogPlan( QueryPlan::make( d, -1, frsp, 0, query, order ) );
        scoped_ptr<FindingStartCursor> finder( FindingStartCursor::make( *oplogPlan ) );
        ElapsedTracker yieldCondition( 256, 20 );
        while( !finder->done() ) {
            if ( yieldCondition.intervalHasElapsed() ) {
                if ( finder->prepareToYield() ) {
                    ClientCursor::staticYield( 0, ns, 0 );
                    finder->recoverFromYield();
                }
            }
            finder->next();
        }
        shared_ptr<Cursor> ret = finder->cursor();
        shared_ptr<CoveredIndexMatcher> matcher( new CoveredIndexMatcher( query, BSONObj() ) );
        ret->setMatcher( matcher );
        return ret;
    }
    
}
