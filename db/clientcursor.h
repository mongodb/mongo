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
    typedef map<CursorId, ClientCursor*> CCById;
    extern CCById clientCursorsById;

    extern BSONObj id_obj;

    class IdSet {
    public:
        IdSet() : mySize_(), inMem_( true ) {}
        ~IdSet() {
            size_ -= mySize_;
        }
        bool get( const BSONObj &id ) const {
            if ( inMem_ )
                return mem_.count( id );
            else
                return db_.get( id );
        }
        void put( const BSONObj &id ) {
            if ( inMem_ ) {
                if ( mem_.insert( id.getOwned() ).second ) {
                    int sizeInc = id.objsize() + sizeof( BSONObj );
                    mySize_ += sizeInc;
                    size_ += sizeInc;
                }
            } else {
                db_.set( id, true );
            }
        }
        void mayUpgradeStorage( const string &name ) {
            if ( size_ > maxSize_ || mySize_ > maxSizePerSet() ) {
                if ( !inMem_ )
                    return;
                log() << "upgrading id set to collection backed storage, cursorid: " << name << endl;
                inMem_ = false;
                db_.reset( "local.temp.clientcursor." + name, id_obj );
                for( BSONObjSetDefaultOrder::const_iterator i = mem_.begin(); i != mem_.end(); ++i )
                    db_.set( *i, true );
                mem_.clear();
                size_ -= mySize_;
                mySize_ = 0;
            }
        }
        bool inMem() const { return inMem_; }
        long long mySize() { return mySize_; }
        static long long aggregateSize() { return size_; }
        
        static long long maxSize_;
    private:
        string name_;
        BSONObjSetDefaultOrder mem_;
        DbSet db_;
        long long mySize_;
        static long long size_;
        long long maxSizePerSet() const { return maxSize_ / 2; }
        
        bool inMem_;
    };
    
    class ClientCursor {
        DiskLoc _lastLoc; // use getter and setter not this.
        static CursorId allocCursorId();
    public:
        ClientCursor() : cursorid( allocCursorId() ), pos(0), idleAgeMillis(0) {
            clientCursorsById.insert( make_pair(cursorid, this) );
        }
        ~ClientCursor();
        const CursorId cursorid;
        string ns;
        auto_ptr<KeyValJSMatcher> matcher;
        auto_ptr<Cursor> c;
        auto_ptr<IdSet> ids_;
        int pos; /* # objects into the cursor so far */
        DiskLoc lastLoc() const {
            return _lastLoc;
        }
        void setLastLoc(DiskLoc);
        auto_ptr< set<string> > filter; // which fields query wants returned
        Message originalMessage; // this is effectively an auto ptr for data the matcher points to
        unsigned idleAgeMillis; // how long has the cursor been around, relative to server idle time

        /* Get rid of cursors for namespaces that begin with nsprefix.
           Used by drop, deleteIndexes, dropDatabase.
        */
        static void invalidate(const char *nsPrefix);

        static bool erase(CursorId id) {
            ClientCursor *cc = find(id);
            if ( cc ) {
                delete cc;
                return true;
            }
            return false;
        }

        static ClientCursor* find(CursorId id, bool warn = true) {
            CCById::iterator it = clientCursorsById.find(id);
            if ( it == clientCursorsById.end() ) {
                if ( warn )
                    OCCASIONALLY out() << "ClientCursor::find(): cursor not found in map " << id << " (ok after a drop)\n";
                return 0;
            }
            return it->second;
        }

        /* call when cursor's location changes so that we can update the
           cursorsbylocation map.  if you are locked and internally iterating, only
           need to call when you are ready to "unlock".
           */
        void updateLocation();

        void cleanupByLocation(DiskLoc loc);
        
        void mayUpgradeStorage() {
            if ( !ids_.get() )
                return;
            stringstream ss;
            ss << ns << "." << cursorid;
            ids_->mayUpgradeStorage( ss.str() );
        }
    };

} // namespace mongo
