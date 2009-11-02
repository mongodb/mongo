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
            if( !goingAway ) {
                // if we are shutting down, we leave this on so potentially we can stop multiple operations
                killCurrentOp = 0;
            }
            uasserted("interrupted");
        }
    }

    /* Query cursors, base class.  This is for our internal cursors.  "ClientCursor" is a separate
       concept and is for the user's cursor.

       WARNING concurrency: the vfunctions below are called back from within a 
       ClientCursor::ccmutex.  Don't cause a deadlock, you've been warned.
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
        virtual BSONObj currKey() const { return BSONObj(); }
        // DiskLoc the cursor requires for continued operation.  Before this
        // DiskLoc is deleted, the cursor must be incremented or destroyed.
        virtual DiskLoc refLoc() = 0;

        /* Implement these if you want the cursor to be "tailable" */
        
        /* Request that the cursor starts tailing after advancing past last record. */
        /* The implementation may or may not honor this request. */
        virtual void setTailable() {}
        /* indicates if tailing is enabled. */
        virtual bool tailable() {
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

        /* used for multikey index traversal to avoid sending back dups. see JSMatcher::matches().
           if a multikey index traversal:
             if loc has already been sent, returns true.
             otherwise, marks loc as sent.
           @param deep - match was against an array, so we know it is multikey.  this is legacy and kept
                         for backwards datafile compatibility.  'deep' can be eliminated next time we 
                         force a data file conversion. 7Jul09
        */
        virtual bool getsetdup(DiskLoc loc) = 0;

        virtual BSONObj prettyStartKey() const { return BSONObj(); }
        virtual BSONObj prettyEndKey() const { return BSONObj(); }

        virtual bool capped() const { return false; }
    };

    // strategy object implementing direction of traversal.
    class AdvanceStrategy {
    public:
        virtual ~AdvanceStrategy() { }
        virtual DiskLoc next( const DiskLoc &prev ) const = 0;
    };

    const AdvanceStrategy *forward();
    const AdvanceStrategy *reverse();

    /* table-scan style cursor */
    class BasicCursor : public Cursor {
    protected:
        DiskLoc curr, last;
        const AdvanceStrategy *s;

    private:
        bool tailable_;
        void init() {
            tailable_ = false;
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
        virtual DiskLoc refLoc() {
            return curr.isNull() ? last : curr;
        }
        
        bool advance() {
            checkForInterrupt();
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

        BasicCursor(DiskLoc dl, const AdvanceStrategy *_s = forward()) : curr(dl), s( _s ) {
            init();
        }
        BasicCursor(const AdvanceStrategy *_s = forward()) : s( _s ) {
            init();
        }
        virtual string toString() {
            return "BasicCursor";
        }
        virtual void setTailable() {
            if ( !curr.isNull() || !last.isNull() )
                tailable_ = true;
        }
        virtual bool tailable() {
            return tailable_;
        }
        virtual bool getsetdup(DiskLoc loc) { return false; }
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
        ForwardCappedCursor( NamespaceDetails *nsd = 0, const DiskLoc &startLoc = DiskLoc() );
        virtual string toString() {
            return "ForwardCappedCursor";
        }
        virtual DiskLoc next( const DiskLoc &prev ) const;
        virtual bool capped() const { return true; }
    private:
        NamespaceDetails *nsd;
    };

    class ReverseCappedCursor : public BasicCursor, public AdvanceStrategy {
    public:
        ReverseCappedCursor( NamespaceDetails *nsd = 0, const DiskLoc &startLoc = DiskLoc() );
        virtual string toString() {
            return "ReverseCappedCursor";
        }
        virtual DiskLoc next( const DiskLoc &prev ) const;
        virtual bool capped() const { return true; }
    private:
        NamespaceDetails *nsd;
    };

} // namespace mongo
