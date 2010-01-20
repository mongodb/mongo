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

#include "stdafx.h"
#include "pdfile.h"
#include "curop.h"

namespace mongo {

    bool BasicCursor::advance() {
        killCurrentOp.checkForInterrupt();
        if ( eof() ) {
            if ( tailable_ && !last.isNull() ) {
                curr = s->next( last );                    
            } else {
                return false;
            }
        } else {
            last = curr;
            curr = s->next( curr );
        }
        return ok();
    }

    /* these will be used outside of mutexes - really functors - thus the const */
    class Forward : public AdvanceStrategy {
        virtual DiskLoc next( const DiskLoc &prev ) const {
            return prev.rec()->getNext( prev );
        }
    } _forward;

    class Reverse : public AdvanceStrategy {
        virtual DiskLoc next( const DiskLoc &prev ) const {
            return prev.rec()->getPrev( prev );
        }
    } _reverse;

    const AdvanceStrategy *forward() {
        return &_forward;
    }
    const AdvanceStrategy *reverse() {
        return &_reverse;
    }

    DiskLoc nextLoop( NamespaceDetails *nsd, const DiskLoc &prev, const DiskLoc &endExt = DiskLoc() ) {
        assert( nsd->capLooped() );
        DiskLoc next = prev.rec()->getNext( prev, endExt );
        if ( !next.isValid() )
            return DiskLoc();
        if ( !next.isNull() )
            return next;
        next = nsd->firstRecord( DiskLoc(), endExt );
        return next.isValid() ? next : DiskLoc();
    }

    DiskLoc prevLoop( NamespaceDetails *nsd, const DiskLoc &curr, const DiskLoc &endExt = DiskLoc() ) {
        assert( nsd->capLooped() );
        DiskLoc prev = curr.rec()->getPrev( curr, endExt );
        if ( !prev.isValid() )
            return DiskLoc();
        if ( !prev.isNull() )
            return prev;
        prev = nsd->lastRecord( DiskLoc(), endExt );
        return prev.isValid() ? prev : DiskLoc();
    }

    ForwardCappedCursor::ForwardCappedCursor( NamespaceDetails *_nsd, const DiskLoc &startLoc ) :
            nsd( _nsd ) {
        if ( !nsd )
            return;
        DiskLoc start = startLoc;
        if ( start.isNull() ) {
            if ( !nsd->capLooped() )
                start = nsd->firstRecord();
            else {
                start = nsd->capExtent.ext()->firstRecord;
                if ( start.isNull() ) { // in this case capFirstNewRecord must be null
                    start = nsd->firstRecord( nsd->capExtent );
                    if ( start.isNull() )
                        start = nsd->firstRecord();
                } else if ( start == nsd->capFirstNewRecord ) {
                    start = nsd->capExtent.ext()->lastRecord;
                    start = nextLoop( nsd, start );
                }
            }
        }
        curr = start;
        s = this;
    }

    DiskLoc ForwardCappedCursor::next( const DiskLoc &prev ) const {
        assert( nsd );
        if ( !nsd->capLooped() )
            return forward()->next( prev );

        DiskLoc i = prev;

        if ( nsd->capFirstNewRecord.isNull() ) {
            return nextLoop( nsd, i, nsd->capExtent );
        }
        
        // Last record
        if ( i == nsd->capExtent.ext()->lastRecord )
            return DiskLoc();
        i = nextLoop( nsd, i );
        // If we become capFirstNewRecord from same extent, advance to next extent.
        if ( i == nsd->capFirstNewRecord && i != nsd->capExtent.ext()->firstRecord )
            i = nextLoop( nsd, nsd->capExtent.ext()->lastRecord );
        // If we have just gotten to beginning of capExtent, skip to capFirstNewRecord
        if ( i == nsd->capExtent.ext()->firstRecord )
            i = nsd->capFirstNewRecord;
        return i;
    }

    ReverseCappedCursor::ReverseCappedCursor( NamespaceDetails *_nsd, const DiskLoc &startLoc ) :
            nsd( _nsd ) {
        if ( !nsd )
            return;
        DiskLoc start = startLoc;
        if ( start.isNull() ) {
            if ( !nsd->capLooped() ) {
                start = nsd->lastRecord();
            } else if( !nsd->capFirstNewRecord.isNull() ) {
                start = nsd->capExtent.ext()->lastRecord;
            } else {
                if ( !nsd->capExtent.ext()->xprev.isNull() )
                    start = nsd->lastRecord( nsd->capExtent.ext()->xprev );
                if ( start.isNull() )
                    start = nsd->lastRecord();
            }
        }
        curr = start;
        s = this;
    }

    DiskLoc ReverseCappedCursor::next( const DiskLoc &prev ) const {
        assert( nsd );
        if ( !nsd->capLooped() )
            return reverse()->next( prev );

        DiskLoc i = prev;

        if ( nsd->capFirstNewRecord.isNull() ) {
            return prevLoop( nsd, i, nsd->capExtent.ext()->xprev.isNull() ? nsd->lastExtent : nsd->capExtent.ext()->xprev );
        }
        
        // Last record
        if ( nsd->capFirstNewRecord == nsd->capExtent.ext()->firstRecord ) {
            if ( i == nextLoop( nsd, nsd->capExtent.ext()->lastRecord ) ) {
                return DiskLoc();
            }
        } else {
            if ( i == nsd->capExtent.ext()->firstRecord ) {
                return DiskLoc();
            }
        }
        // If we are capFirstNewRecord, advance to prev extent, otherwise just get prev.
        if ( i == nsd->capFirstNewRecord )
            i = prevLoop( nsd, nsd->capExtent.ext()->firstRecord );
        else
            i = prevLoop( nsd, i );
        // If we just became last in cap extent, advance past capFirstNewRecord
        // (We know capExtent.ext()->firstRecord != capFirstNewRecord, since would
        // have returned DiskLoc() earlier otherwise.)
        if ( i == nsd->capExtent.ext()->lastRecord )
            i = reverse()->next( nsd->capFirstNewRecord );

        return i;
    }
} // namespace mongo
