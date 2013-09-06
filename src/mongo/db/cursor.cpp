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

#include "mongo/pch.h"

#include "mongo/db/curop-inl.h"
#include "mongo/db/kill_current_op.h"
#include "mongo/db/pdfile.h"

namespace mongo {

    bool BasicCursor::advance() {
        killCurrentOp.checkForInterrupt();
        if ( eof() ) {
            if ( tailable_ && !last.isNull() ) {
                curr = s->next( last );
            }
            else {
                return false;
            }
        }
        else {
            last = curr;
            curr = s->next( curr );
        }
        incNscanned();
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

    DiskLoc nextLoop( NamespaceDetails *nsd, const DiskLoc &prev ) {
        verify( nsd->capLooped() );
        DiskLoc next = forward()->next( prev );
        if ( !next.isNull() )
            return next;
        return nsd->firstRecord();
    }

    DiskLoc prevLoop( NamespaceDetails *nsd, const DiskLoc &curr ) {
        verify( nsd->capLooped() );
        DiskLoc prev = reverse()->next( curr );
        if ( !prev.isNull() )
            return prev;
        return nsd->lastRecord();
    }

    ForwardCappedCursor* ForwardCappedCursor::make( NamespaceDetails* nsd,
                                                    const DiskLoc& startLoc ) {
        auto_ptr<ForwardCappedCursor> ret( new ForwardCappedCursor( nsd ) );
        ret->init( startLoc );
        return ret.release();
    }

    ForwardCappedCursor::ForwardCappedCursor( NamespaceDetails* _nsd ) :
        nsd( _nsd ) {
    }

    void ForwardCappedCursor::init( const DiskLoc& startLoc ) {
        if ( !nsd )
            return;
        DiskLoc start = startLoc;
        if ( start.isNull() ) {
            if ( !nsd->capLooped() )
                start = nsd->firstRecord();
            else {
                start = nsd->capExtent().ext()->firstRecord;
                if ( !start.isNull() && start == nsd->capFirstNewRecord() ) {
                    start = nsd->capExtent().ext()->lastRecord;
                    start = nextLoop( nsd, start );
                }
            }
        }
        curr = start;
        s = this;
        incNscanned();
    }

    DiskLoc ForwardCappedCursor::next( const DiskLoc &prev ) const {
        verify( nsd );
        if ( !nsd->capLooped() )
            return forward()->next( prev );

        DiskLoc i = prev;
        // Last record
        if ( i == nsd->capExtent().ext()->lastRecord )
            return DiskLoc();
        i = nextLoop( nsd, i );
        // If we become capFirstNewRecord from same extent, advance to next extent.
        if ( i == nsd->capFirstNewRecord() &&
             i != nsd->capExtent().ext()->firstRecord )
            i = nextLoop( nsd, nsd->capExtent().ext()->lastRecord );
        // If we have just gotten to beginning of capExtent, skip to capFirstNewRecord
        if ( i == nsd->capExtent().ext()->firstRecord )
            i = nsd->capFirstNewRecord();
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
            }
            else {
                start = nsd->capExtent().ext()->lastRecord;
            }
        }
        curr = start;
        s = this;
        incNscanned();
    }

    DiskLoc ReverseCappedCursor::next( const DiskLoc &prev ) const {
        verify( nsd );
        if ( !nsd->capLooped() )
            return reverse()->next( prev );

        DiskLoc i = prev;
        // Last record
        if ( nsd->capFirstNewRecord() == nsd->capExtent().ext()->firstRecord ) {
            if ( i == nextLoop( nsd, nsd->capExtent().ext()->lastRecord ) ) {
                return DiskLoc();
            }
        }
        else {
            if ( i == nsd->capExtent().ext()->firstRecord ) {
                return DiskLoc();
            }
        }
        // If we are capFirstNewRecord, advance to prev extent, otherwise just get prev.
        if ( i == nsd->capFirstNewRecord() )
            i = prevLoop( nsd, nsd->capExtent().ext()->firstRecord );
        else
            i = prevLoop( nsd, i );
        // If we just became last in cap extent, advance past capFirstNewRecord
        // (We know capExtent.ext()->firstRecord != capFirstNewRecord, since would
        // have returned DiskLoc() earlier otherwise.)
        if ( i == nsd->capExtent().ext()->lastRecord )
            i = reverse()->next( nsd->capFirstNewRecord() );

        return i;
    }
} // namespace mongo
