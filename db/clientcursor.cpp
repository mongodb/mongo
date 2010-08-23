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

#include "pch.h"
#include "query.h"
#include "introspect.h"
#include <time.h>
#include "db.h"
#include "commands.h"
#include "repl_block.h"

namespace mongo {

    typedef multimap<DiskLoc, ClientCursor*> CCByLoc;

    CCById ClientCursor::clientCursorsById;
    boost::recursive_mutex ClientCursor::ccmutex;
    long long ClientCursor::numberTimedOut = 0;

    /*static*/ void ClientCursor::assertNoCursors() { 
        recursive_scoped_lock lock(ccmutex);
        if( clientCursorsById.size() ) { 
            log() << "ERROR clientcursors exist but should not at this point" << endl;
            ClientCursor *cc = clientCursorsById.begin()->second;
            log() << "first one: " << cc->cursorid << ' ' << cc->ns << endl;
            clientCursorsById.clear();
            assert(false);
        }
    }


    void ClientCursor::setLastLoc_inlock(DiskLoc L) {
        if ( L == _lastLoc )
            return;

        CCByLoc& bl = byLoc();
        if ( !_lastLoc.isNull() ) {
            CCByLoc::iterator i = kv_find(bl, _lastLoc, this);
            if ( i != bl.end() )
                bl.erase(i);
        }

        if ( !L.isNull() )
            bl.insert( make_pair(L, this) );
        _lastLoc = L;
    }

    /* ------------------------------------------- */

    /* must call this when a btree node is updated */
    //void removedKey(const DiskLoc& btreeLoc, int keyPos) {
    //}

    /* todo: this implementation is incomplete.  we use it as a prefix for dropDatabase, which
             works fine as the prefix will end with '.'.  however, when used with drop and
    		 dropIndexes, this could take out cursors that belong to something else -- if you
    		 drop "foo", currently, this will kill cursors for "foobar".
    */
    void ClientCursor::invalidate(const char *nsPrefix) {
        vector<ClientCursor*> toDelete;

        int len = strlen(nsPrefix);
        assert( len > 0 && strchr(nsPrefix, '.') );

        {
            recursive_scoped_lock lock(ccmutex);

            Database *db = cc().database();
            assert(db);
            assert( str::startsWith(nsPrefix, db->name) );

            for( CCById::iterator i = clientCursorsById.begin(); i != clientCursorsById.end(); ++i ) {
                ClientCursor *cc = i->second;
                if( cc->_db != db ) 
                    continue;
                if ( strncmp(nsPrefix, cc->ns.c_str(), len) == 0 ) {
                    toDelete.push_back(i->second);
                }
            }

            /*
            note : we can't iterate byloc because clientcursors may exist with a loc of null in which case
                   they are not in the map.  perhaps they should not exist though in the future?  something to 
                   change???
                   
            CCByLoc& bl = db->ccByLoc;
            for ( CCByLoc::iterator i = bl.begin(); i != bl.end(); ++i ) {
                ClientCursor *cc = i->second;
                if ( strncmp(nsPrefix, cc->ns.c_str(), len) == 0 ) {
                    assert( cc->_db == db );
                    toDelete.push_back(i->second);
                }
            }*/

            for ( vector<ClientCursor*>::iterator i = toDelete.begin(); i != toDelete.end(); ++i )
                delete (*i);
        }
    }

    bool ClientCursor::shouldTimeout( unsigned millis ){
        _idleAgeMillis += millis;
        return _idleAgeMillis > 600000 && _pinValue == 0;
    }

    /* called every 4 seconds.  millis is amount of idle time passed since the last call -- could be zero */
    void ClientCursor::idleTimeReport(unsigned millis) {
        readlock lk("");
        recursive_scoped_lock lock(ccmutex);
        for ( CCById::iterator i = clientCursorsById.begin(); i != clientCursorsById.end();  ) {
            CCById::iterator j = i;
            i++;
            if( j->second->shouldTimeout( millis ) ){
                numberTimedOut++;
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
        recursive_scoped_lock lock(ccmutex);
        Database *db = cc().database();
        CCByLoc& bl = db->ccByLoc;
        RARELY if ( bl.size() > 70 ) {
            log() << "perf warning: byLoc.size=" << bl.size() << " in aboutToDeleteBucket\n";
        }
        for ( CCByLoc::iterator i = bl.begin(); i != bl.end(); i++ )
            i->second->c->aboutToDeleteBucket(b);
    }
    void aboutToDeleteBucket(const DiskLoc& b) {
        ClientCursor::informAboutToDeleteBucket(b); 
    }

    /* must call this on a delete so we clean up the cursors. */
    void ClientCursor::aboutToDelete(const DiskLoc& dl) {
        recursive_scoped_lock lock(ccmutex);

        Database *db = cc().database();
        assert(db);
        CCByLoc& bl = db->ccByLoc;
        CCByLoc::iterator j = bl.lower_bound(dl);
        CCByLoc::iterator stop = bl.upper_bound(dl);
        if ( j == stop )
            return;

        vector<ClientCursor*> toAdvance;

        while ( 1 ) {
            toAdvance.push_back(j->second);
            DEV assert( j->first == dl );
            ++j;
            if ( j == stop )
                break;
        }

        wassert( toAdvance.size() < 5000 );
        
        for ( vector<ClientCursor*>::iterator i = toAdvance.begin(); i != toAdvance.end(); ++i ){
            ClientCursor* cc = *i;
            wassert(cc->_db == db);
            
            if ( cc->_doingDeletes ) continue;

            Cursor *c = cc->c.get();
            if ( c->capped() ){
                delete cc;
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
                // advanced to end
                // leave ClientCursor in place so next getMore doesn't fail
                // still need to mark new location though
                cc->updateLocation();
            }
            else {
                wassert( c->refLoc() != dl );
                cc->updateLocation();
            }
        }
    }
    void aboutToDelete(const DiskLoc& dl) { ClientCursor::aboutToDelete(dl); }

    ClientCursor::~ClientCursor() {
        assert( pos != -2 );

        {
            recursive_scoped_lock lock(ccmutex);
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
        } else {
            recursive_scoped_lock lock(ccmutex);
            setLastLoc_inlock(cl);
        }
        // may be necessary for MultiCursor even when cl hasn't changed
        c->noteLocation();
    }
    
    int ClientCursor::yieldSuggest() {
        int writers = 0;
        int readers = 0;
        
        int micros = Client::recommendedYieldMicros( &writers , &readers );
        
        if ( micros > 0 && writers == 0 && dbMutex.getState() <= 0 ){
            // we have a read lock, and only reads are coming on, so why bother unlocking
            micros = 0;
        }
        
        return micros;
    }
    
    bool ClientCursor::yieldSometimes(){
        if ( ! _yieldSometimesTracker.ping() )
            return true;

        int micros = yieldSuggest();
        return ( micros > 0 ) ? yield( micros ) : true;
    }

    void ClientCursor::staticYield( int micros ) {
        {
            dbtempreleasecond unlock;
            if ( unlock.unlocked() ){
                if ( micros == -1 )
                    micros = Client::recommendedYieldMicros();
                if ( micros > 0 )
                    sleepmicros( micros ); 
            }
            else {
                log( LL_WARNING ) << "ClientCursor::yield can't unlock b/c of recursive lock" << endl;
            }
        }        
    }
    
    bool ClientCursor::prepareToYield( YieldData &data ) {
        if ( ! c->supportYields() )
            return false;
        // need to store in case 'this' gets deleted
        data._id = cursorid;
        
        data._doingDeletes = _doingDeletes;
        _doingDeletes = false;
        
        updateLocation();
        
        {
            /* a quick test that our temprelease is safe. 
             todo: make a YieldingCursor class 
             and then make the following code part of a unit test.
             */
            const int test = 0;
            static bool inEmpty = false;
            if( test && !inEmpty ) { 
                inEmpty = true;
                log() << "TEST: manipulate collection during cc:yield" << endl;
                if( test == 1 ) 
                    Helpers::emptyCollection(ns.c_str());
                else if( test == 2 ) {
                    BSONObjBuilder b; string m;
                    dropCollection(ns.c_str(), m, b);
                }
                else { 
                    dropDatabase(ns.c_str());
                }
            }
        }        
        return true;
    }
    
    bool ClientCursor::recoverFromYield( const YieldData &data ) {
        ClientCursor *cc = ClientCursor::find( data._id , false );
        if ( cc == 0 ){
            // id was deleted
            return false;
        }
        
        cc->_doingDeletes = data._doingDeletes;
        cc->c->checkLocation();
        return true;        
    }
    
    bool ClientCursor::yield( int micros ) {
        if ( ! c->supportYields() )
            return true;
        YieldData data; 
        prepareToYield( data );
        
        staticYield( micros );

        return ClientCursor::recoverFromYield( data );
    }

    int ctmLast = 0; // so we don't have to do find() which is a little slow very often.
    long long ClientCursor::allocCursorId_inlock() {
        if( 0 ) { 
            static long long z;
            ++z;
            cout << "TEMP alloccursorid " << z << endl;
            return z;
        }

        long long x;
        int ctm = (int) curTimeMillis();
        while ( 1 ) {
            x = (((long long)rand()) << 32);
            x = x | ctm | 0x80000000; // OR to make sure not zero
            if ( ctm != ctmLast || ClientCursor::find_inlock(x, false) == 0 )
                break;
        }
        ctmLast = ctm;
        //DEV tlog() << "  alloccursorid " << x << endl;
        return x;
    }

    void ClientCursor::storeOpForSlave( DiskLoc last ){
        if ( ! ( _queryOptions & QueryOption_OplogReplay ))
            return;

        if ( last.isNull() )
            return;
        
        BSONElement e = last.obj()["ts"];
        if ( e.type() == Date || e.type() == Timestamp )
            _slaveReadTill = e._opTime();
    }
    
    void ClientCursor::updateSlaveLocation( CurOp& curop ){
        if ( _slaveReadTill.isNull() )
            return;
        mongo::updateSlaveLocation( curop , ns.c_str() , _slaveReadTill );
    }


    void ClientCursor::appendStats( BSONObjBuilder& result ){
        recursive_scoped_lock lock(ccmutex);
        result.appendNumber("totalOpen", clientCursorsById.size() );
        result.appendNumber("clientCursors_size", numCursors());
        result.appendNumber("timedOut" , numberTimedOut);
    }
    
    // QUESTION: Restrict to the namespace from which this command was issued?
    // Alternatively, make this command admin-only?
    class CmdCursorInfo : public Command {
    public:
        CmdCursorInfo() : Command( "cursorInfo", true ) {}
        virtual bool slaveOk() const { return true; }
        virtual void help( stringstream& help ) const {
            help << " example: { cursorInfo : 1 }";
        }
        virtual LockType locktype() const { return NONE; }
        bool run(const string& dbname, BSONObj& jsobj, string& errmsg, BSONObjBuilder& result, bool fromRepl ){
            ClientCursor::appendStats( result );
            return true;
        }
    } cmdCursorInfo;
    
    void ClientCursorMonitor::run(){
        Client::initThread("clientcursormon");
        Client& client = cc();
        
        unsigned old = curTimeMillis();

        while ( ! inShutdown() ){
            unsigned now = curTimeMillis();
            ClientCursor::idleTimeReport( now - old );
            old = now;
            sleepsecs(4);
        }

        client.shutdown();
    }

    void ClientCursor::find( const string& ns , set<CursorId>& all ){
        recursive_scoped_lock lock(ccmutex);
        
        for ( CCById::iterator i=clientCursorsById.begin(); i!=clientCursorsById.end(); ++i ){
            if ( i->second->ns == ns )
                all.insert( i->first );
        }
    }


    ClientCursorMonitor clientCursorMonitor;

} // namespace mongo
