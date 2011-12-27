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

#include "../pch.h"

#include "jsobj.h"
#include "diskloc.h"
#include "matcher.h"

namespace mongo {

    class NamespaceDetails;
    class Record;
    class CoveredIndexMatcher;

    /* Query cursors, base class.  This is for our internal cursors.  "ClientCursor" is a separate
       concept and is for the user's cursor.

       WARNING concurrency: the vfunctions below are called back from within a
       ClientCursor::ccmutex.  Don't cause a deadlock, you've been warned.
    */
    class Cursor : boost::noncopyable {
    public:
        virtual ~Cursor() {}
        virtual bool ok() = 0;
        bool eof() { return !ok(); }
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

        virtual bool supportGetMore() = 0;

        /* called after every query block is iterated -- i.e. between getMore() blocks
           so you can note where we are, if necessary.
           */
        virtual void noteLocation() { }

        /* called before query getmore block is iterated */
        virtual void checkLocation() { }

        /**
         * Called before a document pointed at by an earlier iterate of this cursor is to be
         * modified.  It is ok if the current iterate also points to the document to be modified.
         */
        virtual void prepareToTouchEarlierIterate() { noteLocation(); }

        /** Recover from a previous call to prepareToTouchEarlierIterate(). */
        virtual void recoverFromTouchingEarlierIterate() { checkLocation(); }

        virtual bool supportYields() = 0;

        /** Called before a ClientCursor yield. */
        virtual bool prepareToYield() { noteLocation(); return supportYields(); }
        
        /** Called after a ClientCursor yield.  Recovers from a previous call to prepareToYield(). */
        virtual void recoverFromYield() { checkLocation(); }

        virtual string toString() { return "abstract?"; }

        /* used for multikey index traversal to avoid sending back dups. see Matcher::matches().
           if a multikey index traversal:
             if loc has already been sent, returns true.
             otherwise, marks loc as sent.
        */
        virtual bool getsetdup(DiskLoc loc) = 0;

        virtual bool isMultiKey() const = 0;

        virtual bool autoDedup() const { return true; }

        /**
         * return true if the keys in the index have been modified from the main doc
         * if you have { a : 1 , b : [ 1 , 2 ] }
         * an index on { a : 1 } would not be modified
         * an index on { b : 1 } would be since the values of the array are put in the index
         *                       not the array
         */
        virtual bool modifiedKeys() const = 0;

        virtual BSONObj prettyIndexBounds() const { return BSONArray(); }

        /**
         * If true, this is an unindexed cursor over a capped collection.  Currently such cursors must
         * not own a delegate ClientCursor, due to the implementation of ClientCursor::aboutToDelete(). - SERVER-4563
         */
        virtual bool capped() const { return false; }

        virtual long long nscanned() = 0;

        // The implementation may return different matchers depending on the
        // position of the cursor.  If matcher() is nonzero at the start,
        // matcher() should be checked each time advance() is called.
        // Implementations which generate their own matcher should return this
        // to avoid a matcher being set manually.
        // Note that the return values differ subtly here

        // Used when we want fast matcher lookup
        virtual CoveredIndexMatcher *matcher() const { return 0; }
        // Used when we need to share this matcher with someone else
        virtual shared_ptr< CoveredIndexMatcher > matcherPtr() const { return shared_ptr< CoveredIndexMatcher >(); }

        virtual bool currentMatches( MatchDetails *details = 0 ) {
            return !matcher() || matcher()->matchesCurrent( this, details );
        }

        // A convenience function for setting the value of matcher() manually
        // so it may accessed later.  Implementations which must generate
        // their own matcher() should assert here.
        virtual void setMatcher( shared_ptr< CoveredIndexMatcher > matcher ) {
            massert( 13285, "manual matcher config not allowed", false );
        }

        virtual void explainDetails( BSONObjBuilder& b ) { return; }
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
    public:
        BasicCursor(DiskLoc dl, const AdvanceStrategy *_s = forward()) : curr(dl), s( _s ), _nscanned() {
            incNscanned();
            init();
        }
        BasicCursor(const AdvanceStrategy *_s = forward()) : s( _s ), _nscanned() {
            init();
        }
        bool ok() { return !curr.isNull(); }
        Record* _current() {
            assert( ok() );
            return curr.rec();
        }
        BSONObj current() {
            Record *r = _current();
            BSONObj j(r);
            return j;
        }
        virtual DiskLoc currLoc() { return curr; }
        virtual DiskLoc refLoc()  { return curr.isNull() ? last : curr; }
        bool advance();
        virtual string toString() { return "BasicCursor"; }
        virtual void setTailable() {
            if ( !curr.isNull() || !last.isNull() )
                tailable_ = true;
        }
        virtual bool tailable() { return tailable_; }
        virtual bool getsetdup(DiskLoc loc) { return false; }
        virtual bool isMultiKey() const { return false; }
        virtual bool modifiedKeys() const { return false; }
        virtual bool supportGetMore() { return true; }
        virtual bool supportYields() { return true; }
        virtual CoveredIndexMatcher *matcher() const { return _matcher.get(); }
        virtual shared_ptr< CoveredIndexMatcher > matcherPtr() const { return _matcher; }
        virtual void setMatcher( shared_ptr< CoveredIndexMatcher > matcher ) { _matcher = matcher; }
        virtual long long nscanned() { return _nscanned; }

    protected:
        DiskLoc curr, last;
        const AdvanceStrategy *s;
        void incNscanned() { if ( !curr.isNull() ) { ++_nscanned; } }
    private:
        bool tailable_;
        shared_ptr< CoveredIndexMatcher > _matcher;
        long long _nscanned;
        void init() { tailable_ = false; }
    };

    /* used for order { $natural: -1 } */
    class ReverseCursor : public BasicCursor {
    public:
        ReverseCursor(DiskLoc dl) : BasicCursor( dl, reverse() ) { }
        ReverseCursor() : BasicCursor( reverse() ) { }
        virtual string toString() { return "ReverseCursor"; }
    };

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
