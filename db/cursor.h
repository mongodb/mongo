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

#include "../stdafx.h"

#include "jsobj.h"
#include "storage.h"

namespace mongo {
    
    class Record;

    /* 0 = ok
       1 = kill current operation and reset this to 0
       future: maybe use this as a "going away" thing on process termination with a higher flag value 
    */
    extern int killCurrentOp;

    inline void checkForInterrupt() {
        if( killCurrentOp ) { 
            killCurrentOp = 0;
            uasserted("interrupted");
        }
    }

    /* Query cursors, base class.  This is for our internal cursors.  "ClientCursor" is a separate
       concept and is for the user's cursor.
    */
    class Cursor {
    public:
        virtual ~Cursor() {}
        virtual bool ok() = 0;
        bool eof() {
            return !ok();
        }
        virtual Record* _current() = 0;
        virtual BSONObj current() = 0;
        virtual DiskLoc currLoc() = 0;
        virtual bool advance() = 0; /*true=ok*/
        virtual BSONObj currKey() const { return emptyObj; }

        /* Implement these if you want the cursor to be "tailable" */
        /* tailable(): if true, cursor has tailable capability AND
                       the user requested use of those semantics. */
        virtual bool tailable() {
            return false;
        }
        /* indicates we should mark where we are and go into tail mode. */
        virtual void setAtTail() {
            assert(false);
        }
        /* you must call tailResume before reusing the cursor */
        virtual void tailResume() { }
        /* indicates ifi we are actively tailing.  once it goes active,
           this should return treu even after tailResume(). */
        virtual bool tailing() {
            return false;
        }

        virtual void aboutToDeleteBucket(const DiskLoc& b) { }

        /* optional to implement.  if implemented, means 'this' is a prototype */
        virtual Cursor* clone() {
            return 0;
        }

        virtual BSONObj indexKeyPattern() {
            return BSONObj();
        }

        /* called after every query block is iterated -- i.e. between getMore() blocks
           so you can note where we are, if necessary.
           */
        virtual void noteLocation() { }

        /* called before query getmore block is iterated */
        virtual void checkLocation() { }

        virtual string toString() {
            return "abstract?";
        }

        /* used for multikey index traversal to avoid sending back dups. see JSMatcher::matches() */
        set<DiskLoc> dups;
        bool getsetdup(DiskLoc loc) {
            /* to save mem only call this when there is risk of dups (e.g. when 'deep'/multikey) */
            if ( dups.count(loc) > 0 )
                return true;
            dups.insert(loc);
            return false;
        }

        virtual BSONObj prettyStartKey() const { return emptyObj; }
        virtual BSONObj prettyEndKey() const { return emptyObj; }
    };

    class AdvanceStrategy {
    public:
        virtual ~AdvanceStrategy() {}
        virtual DiskLoc next( const DiskLoc &prev ) const = 0;
    };

    AdvanceStrategy *forward();
    AdvanceStrategy *reverse();

    /* table-scan style cursor */
    class BasicCursor : public Cursor {
    protected:
        DiskLoc curr, last;
        AdvanceStrategy *s;

    private:
        // for tailing:
        enum State { Normal, TailPoint, TailResumed } state;
        void init() {
            state = Normal;
        }

    public:
        bool ok() {
            return !curr.isNull();
        }
        Record* _current() {
            assert( ok() );
            return curr.rec();
        }
        BSONObj current() {
            Record *r = _current();
            BSONObj j(r);
            return j;
        }
        virtual DiskLoc currLoc() {
            return curr;
        }

        bool advance() {
            checkForInterrupt();
            if ( eof() )
                return false;
            _current();
            last = curr;
            curr = s->next( curr );
            return ok();
        }

        BasicCursor(DiskLoc dl, AdvanceStrategy *_s = forward()) : curr(dl), s( _s ) {
            init();
        }
        BasicCursor(AdvanceStrategy *_s = forward()) : s( _s ) {
            init();
        }
        virtual string toString() {
            return "BasicCursor";
        }

        virtual void tailResume() {
            if ( state == TailPoint ) {
                state = TailResumed;
                advance();
            }
        }
        virtual void setAtTail() {
            assert( state != TailPoint );
            assert( curr.isNull() );
            assert( !last.isNull() );
            curr = last;
            last.Null();
            state = TailPoint;
        }
        virtual bool tailable() {
            // to go into tail mode we need a non-null point of reference for resumption
            return !last.isNull();
        }
        virtual bool tailing() {
            return state != Normal;
        }
    };

    /* used for order { $natural: -1 } */
    class ReverseCursor : public BasicCursor {
    public:
        ReverseCursor(DiskLoc dl) : BasicCursor( dl, reverse() ) { }
        ReverseCursor() : BasicCursor( reverse() ) { }
        virtual string toString() {
            return "ReverseCursor";
        }
    };

    class NamespaceDetails;

    class ForwardCappedCursor : public BasicCursor, public AdvanceStrategy {
    public:
        ForwardCappedCursor( NamespaceDetails *nsd = 0 );
        virtual string toString() {
            return "ForwardCappedCursor";
        }
        virtual DiskLoc next( const DiskLoc &prev ) const;
    private:
        NamespaceDetails *nsd;
    };

    class ReverseCappedCursor : public BasicCursor, public AdvanceStrategy {
    public:
        ReverseCappedCursor( NamespaceDetails *nsd = 0 );
        virtual string toString() {
            return "ReverseCappedCursor";
        }
        virtual DiskLoc next( const DiskLoc &prev ) const;
    private:
        NamespaceDetails *nsd;
    };

} // namespace mongo
