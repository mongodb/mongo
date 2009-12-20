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

/* clientcursor.cpp

   ClientCursor is a wrapper that represents a cursorid from our database
   application's perspective.

   Cursor -- and its derived classes -- are our internal cursors.
*/

#include "stdafx.h"
#include "query.h"
#include "introspect.h"
#include <time.h>
#include "db.h"
#include "commands.h"

namespace mongo {

    CCById ClientCursor::clientCursorsById;
    CCByLoc ClientCursor::byLoc;
    boost::recursive_mutex ClientCursor::ccmutex;

    unsigned ClientCursor::byLocSize() { 
        recursive_boostlock lock(ccmutex);
        return byLoc.size();
    }

    void ClientCursor::setLastLoc_inlock(DiskLoc L) {
        if ( L == _lastLoc )
            return;

        if ( !_lastLoc.isNull() ) {
            CCByLoc::iterator i = kv_find(byLoc, _lastLoc, this);
            if ( i != byLoc.end() )
                byLoc.erase(i);
        }

        if ( !L.isNull() )
            byLoc.insert( make_pair(L, this) );
        _lastLoc = L;
    }

    /* ------------------------------------------- */

    /* must call this when a btree node is updated */
    //void removedKey(const DiskLoc& btreeLoc, int keyPos) {
    //}

    /* todo: this implementation is incomplete.  we use it as a prefix for dropDatabase, which
             works fine as the prefix will end with '.'.  however, when used with drop and
    		 deleteIndexes, this could take out cursors that belong to something else -- if you
    		 drop "foo", currently, this will kill cursors for "foobar".
    */
    void ClientCursor::invalidate(const char *nsPrefix) {
        vector<ClientCursor*> toDelete;

        int len = strlen(nsPrefix);
        assert( len > 0 && strchr(nsPrefix, '.') );

        {
            recursive_boostlock lock(ccmutex);

            for ( CCByLoc::iterator i = byLoc.begin(); i != byLoc.end(); ++i ) {
                ClientCursor *cc = i->second;
                if ( strncmp(nsPrefix, cc->ns.c_str(), len) == 0 )
                    toDelete.push_back(i->second);
            }

            for ( vector<ClientCursor*>::iterator i = toDelete.begin(); i != toDelete.end(); ++i )
                delete (*i);
        }
    }

    /* called every 4 seconds.  millis is amount of idle time passed since the last call -- could be zero */
    void ClientCursor::idleTimeReport(unsigned millis) {
        recursive_boostlock lock(ccmutex);
        for ( CCByLoc::iterator i = byLoc.begin(); i != byLoc.end();  ) {
            CCByLoc::iterator j = i;
            i++;
            if( j->second->shouldTimeout( millis ) ){
                log(1) << "killing old cursor " << j->second->cursorid << ' ' << j->second->ns 
                       << " idle:" << j->second->idleTime() << "ms\n";
                delete j->second;
            }
        }
    }

    /* must call when a btree bucket going away.
       note this is potentially slow
    */
    void ClientCursor::informAboutToDeleteBucket(const DiskLoc& b) {
        recursive_boostlock lock(ccmutex);
        RARELY if ( byLoc.size() > 70 ) {
            log() << "perf warning: byLoc.size=" << byLoc.size() << " in aboutToDeleteBucket\n";
        }
        for ( CCByLoc::iterator i = byLoc.begin(); i != byLoc.end(); i++ )
            i->second->c->aboutToDeleteBucket(b);
    }
    void aboutToDeleteBucket(const DiskLoc& b) {
        ClientCursor::informAboutToDeleteBucket(b); 
    }

    /* must call this on a delete so we clean up the cursors. */
    void ClientCursor::aboutToDelete(const DiskLoc& dl) {
        recursive_boostlock lock(ccmutex);

        CCByLoc::iterator j = byLoc.lower_bound(dl);
        CCByLoc::iterator stop = byLoc.upper_bound(dl);
        if ( j == stop )
            return;

        vector<ClientCursor*> toAdvance;

        while ( 1 ) {
            toAdvance.push_back(j->second);
            WIN assert( j->first == dl );
            ++j;
            if ( j == stop )
                break;
        }

        wassert( toAdvance.size() < 5000 );

        for ( vector<ClientCursor*>::iterator i = toAdvance.begin();
                i != toAdvance.end(); ++i )
        {
            Cursor *c = (*i)->c.get();
            if ( c->capped() ){
                delete *i;
                continue;
            }
            
            c->checkLocation();
            DiskLoc tmp1 = c->refLoc();
            if ( tmp1 != dl ) {
                /* this might indicate a failure to call ClientCursor::updateLocation() */
                problem() << "warning: cursor loc " << tmp1 << " does not match byLoc position " << dl << " !" << endl;
            }
            c->advance();
            if ( c->eof() ) {
                // advanced to end -- delete cursor
                delete *i;
            }
            else {
                wassert( c->refLoc() != dl );
                (*i)->updateLocation();
            }
        }
    }
    void aboutToDelete(const DiskLoc& dl) { ClientCursor::aboutToDelete(dl); }

    ClientCursor::~ClientCursor() {
        assert( pos != -2 );

        {
            recursive_boostlock lock(ccmutex);
            setLastLoc_inlock( DiskLoc() ); // removes us from bylocation multimap
            clientCursorsById.erase(cursorid);

            // defensive:
            (CursorId&) cursorid = -1;
            pos = -2;
        }
    }

    /* call when cursor's location changes so that we can update the
       cursorsbylocation map.  if you are locked and internally iterating, only
       need to call when you are ready to "unlock".
    */
    void ClientCursor::updateLocation() {
        assert( cursorid );
        _idleAgeMillis = 0;
        DiskLoc cl = c->refLoc();
        if ( lastLoc() == cl ) {
            //log() << "info: lastloc==curloc " << ns << '\n';
            return;
        }
        {
            recursive_boostlock lock(ccmutex);
            setLastLoc_inlock(cl);
            c->noteLocation();
        }
    }

    int ctmLast = 0; // so we don't have to do find() which is a little slow very often.
    long long ClientCursor::allocCursorId_inlock() {
        long long x;
        int ctm = (int) curTimeMillis();
        while ( 1 ) {
            x = (((long long)rand()) << 32);
            x = x | ctm | 0x80000000; // OR to make sure not zero
            if ( ctm != ctmLast || ClientCursor::find_inlock(x, false) == 0 )
                break;
        }
        ctmLast = ctm;
        DEV out() << "  alloccursorid " << x << endl;
        return x;
    }

    // QUESTION: Restrict to the namespace from which this command was issued?
    // Alternatively, make this command admin-only?
    class CmdCursorInfo : public Command {
    public:
        CmdCursorInfo() : Command( "cursorInfo" ) {}
        virtual bool slaveOk() { return true; }
        virtual void help( stringstream& help ) const {
            help << " example: { cursorInfo : 1 }";
        }
        bool run(const char *dbname, BSONObj& jsobj, string& errmsg, BSONObjBuilder& result, bool fromRepl ){
            recursive_boostlock lock(ClientCursor::ccmutex);
            result.append("byLocation_size", unsigned( ClientCursor::byLoc.size() ) );
            result.append("clientCursors_size", unsigned( ClientCursor::clientCursorsById.size() ) );
            return true;
        }
    } cmdCursorInfo;

} // namespace mongo
