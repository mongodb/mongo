/* clientcursor.h */

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

/* Cursor -- and its derived classes -- are our internal cursors.

   ClientCursor is a wrapper that represents a cursorid from our database
   application's perspective.
*/

#pragma once

#include "../stdafx.h"
#include "cursor.h"
#include "jsobj.h"
#include "../util/message.h"
#include "storage.h"
#include "dbhelpers.h"
#include "matcher.h"

namespace mongo {

    typedef long long CursorId; /* passed to the client so it can send back on getMore */
    class Cursor; /* internal server cursor base class */
    class ClientCursor;

    /* todo: make this map be per connection.  this will prevent cursor hijacking security attacks perhaps.
    */
    typedef map<CursorId, ClientCursor*> CCById;

    typedef multimap<DiskLoc, ClientCursor*> CCByLoc;

    extern BSONObj id_obj;

    class ClientCursor {
        friend class CmdCursorInfo;
        DiskLoc _lastLoc;                        // use getter and setter not this (important)
        unsigned _idleAgeMillis;                 // how long has the cursor been around, relative to server idle time

        /* 0 = normal
           1 = no timeout allowed
           100 = in use (pinned) -- see Pointer class
        */
        unsigned _pinValue;

        bool _doingDeletes;

        static CCById clientCursorsById;
        static CCByLoc byLoc;
        static boost::recursive_mutex ccmutex;   // must use this for all statics above!
        
        static CursorId allocCursorId_inlock();

    public:
        /* use this to assure we don't in the background time out cursor while it is under use.
           if you are using noTimeout() already, there is no risk anyway.
           Further, this mechanism guards against two getMore requests on the same cursor executing
           at the same time - which might be bad.  That should never happen, but if a client driver
           had a bug, it could (or perhaps some sort of attack situation).
        */
        class Pointer : boost::noncopyable { 
        public:
            ClientCursor *_c;
            void release() {
                if( _c ) {
                    assert( _c->_pinValue >= 100 );
                    _c->_pinValue -= 100;
                }
                _c = 0;
            }
            Pointer(long long cursorid) {
                recursive_boostlock lock(ccmutex);
                _c = ClientCursor::find_inlock(cursorid, true);
                if( _c ) {
                    if( _c->_pinValue >= 100 ) {
                        _c = 0;
                        uassert(12051, "clientcursor already in use? driver problem?", false);
                    }
                    _c->_pinValue += 100;
                }
            }
            ~Pointer() {
                release();
            }
        }; 

        /*const*/ CursorId cursorid;
        string ns;
        auto_ptr<CoveredIndexMatcher> matcher;
        auto_ptr<Cursor> c;
        int pos;                                 // # objects into the cursor so far 
        BSONObj query;

        ClientCursor() : _idleAgeMillis(0), _pinValue(0), _doingDeletes(false), pos(0) {
            recursive_boostlock lock(ccmutex);
            cursorid = allocCursorId_inlock();
            clientCursorsById.insert( make_pair(cursorid, this) );
        }
        ~ClientCursor();

        DiskLoc lastLoc() const {
            return _lastLoc;
        }

        auto_ptr< FieldMatcher > filter; // which fields query wants returned
        Message originalMessage; // this is effectively an auto ptr for data the matcher points to

        /* Get rid of cursors for namespaces that begin with nsprefix.
           Used by drop, deleteIndexes, dropDatabase.
        */
        static void invalidate(const char *nsPrefix);

        /**
         * do a dbtemprelease 
         * note: caller should check matcher.docMatcher().atomic() first and not yield if atomic - 
         *       we don't do herein as this->matcher (above) is only initialized for true queries/getmore.
         *       (ie not set for remote/update)
         * @return if the cursor is still valid. 
         *         if false is returned, then this ClientCursor should be considered deleted
         */
        bool yield();
    private:
        void setLastLoc_inlock(DiskLoc);

        static ClientCursor* find_inlock(CursorId id, bool warn = true) {
            CCById::iterator it = clientCursorsById.find(id);
            if ( it == clientCursorsById.end() ) {
                if ( warn )
                    OCCASIONALLY out() << "ClientCursor::find(): cursor not found in map " << id << " (ok after a drop)\n";
                return 0;
            }
            return it->second;
        }
    public:
        static ClientCursor* find(CursorId id, bool warn = true) { 
            recursive_boostlock lock(ccmutex);
            ClientCursor *c = find_inlock(id, warn);
            assert( c->_pinValue ); // otherwise, not thread safe
            return c;
        }

        static bool erase(CursorId id) {
            recursive_boostlock lock(ccmutex);
            ClientCursor *cc = find_inlock(id);
            if ( cc ) {
                assert( cc->_pinValue < 100 ); // you can't still have an active ClientCursor::Pointer
                delete cc;
                return true;
            }
            return false;
        }

        /* call when cursor's location changes so that we can update the
           cursorsbylocation map.  if you are locked and internally iterating, only
           need to call when you are ready to "unlock".
           */
        void updateLocation();

        void cleanupByLocation(DiskLoc loc);
        
        void mayUpgradeStorage() {
            /* if ( !ids_.get() )
                return;
            stringstream ss;
            ss << ns << "." << cursorid;
            ids_->mayUpgradeStorage( ss.str() );*/
        }

        /**
         * @param millis amount of idle passed time since last call
         */
        bool shouldTimeout( unsigned millis ){
            _idleAgeMillis += millis;
            return _idleAgeMillis > 600000 && _pinValue == 0;
        }
        
        unsigned idleTime(){
            return _idleAgeMillis;
        }

        static void idleTimeReport(unsigned millis);

        // cursors normally timeout after an inactivy period to prevent excess memory use
        // setting this prevents timeout of the cursor in question.
        void noTimeout() { 
            _pinValue++;
        }

        void setDoingDeletes( bool doingDeletes ){
            _doingDeletes = doingDeletes;
        }

        static unsigned byLocSize();        // just for diagnostics

        static void informAboutToDeleteBucket(const DiskLoc& b);
        static void aboutToDelete(const DiskLoc& dl);
    };

    
} // namespace mongo
