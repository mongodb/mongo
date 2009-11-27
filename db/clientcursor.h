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
        bool _liveForever;                       // if true, never time out cursor
        
        static CCById clientCursorsById;
        static CCByLoc byLoc;
        static boost::recursive_mutex ccmutex;   // must use this for all statics above!

        static CursorId allocCursorId_inlock();

    public:
        /*const*/ CursorId cursorid;
        string ns;
        auto_ptr<KeyValJSMatcher> matcher;
        auto_ptr<Cursor> c;
        int pos;                                 // # objects into the cursor so far 
        BSONObj query;

        ClientCursor() : _idleAgeMillis(0), _liveForever(false), pos(0) {
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
            return find_inlock(id, warn);
        }

        static bool erase(CursorId id) {
            recursive_boostlock lock(ccmutex);
            ClientCursor *cc = find_inlock(id);
            if ( cc ) {
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
            return ! _liveForever && _idleAgeMillis > 600000;
        }
        
        unsigned idleTime(){
            return _idleAgeMillis;
        }

        static void idleTimeReport(unsigned millis);

        void liveForever() {
            _liveForever = true;
        }

        static unsigned byLocSize();        // just for diagnostics
//        static void idleTimeReport(unsigned millis);

        static void informAboutToDeleteBucket(const DiskLoc& b);
        static void aboutToDelete(const DiskLoc& dl);
    };

    
} // namespace mongo
