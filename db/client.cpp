// client.cpp

/**
*    Copyright (C) 2009 10gen Inc.
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

/* Client represents a connection to the database (the server-side) and corresponds 
   to an open socket (or logical connection if pooling on sockets) from a client.
*/

#include "stdafx.h"
#include "db.h"
#include "client.h"
#include "curop.h"
#include "json.h"
#include "security.h"

namespace mongo {

    boost::mutex Client::clientsMutex;
    set<Client*> Client::clients; // always be in clientsMutex when manipulating this
    boost::thread_specific_ptr<Client> currentClient;

    Client::Client(const char *desc) : 
      _context(0),
      _shutdown(false),
      _desc(desc),
      _god(0)
    {
        _curOp = new CurOp( this );
        boostlock bl(clientsMutex);
        clients.insert(this);
    }

    Client::~Client() { 
        delete _curOp;
        _god = 0;

        if ( _context )
            cout << "ERROR: Client::~Client _context should be NULL" << endl;
        if ( !_shutdown ) 
            cout << "ERROR: Client::shutdown not called: " << _desc << endl;
    }

    bool Client::shutdown(){
        _shutdown = true;
        if ( inShutdown() )
            return false;
        {
            boostlock bl(clientsMutex);
            clients.erase(this);
        }

        bool didAnything = false;
        
        if ( _tempCollections.size() ){
            didAnything = true;
            for ( list<string>::iterator i = _tempCollections.begin(); i!=_tempCollections.end(); i++ ){
                string ns = *i;
                dblock l;
                Client::Context ctx( ns );
                if ( ! nsdetails( ns.c_str() ) )
                    continue;
                try {
                    string err;
                    BSONObjBuilder b;
                    dropCollection( ns , err , b );
                }
                catch ( ... ){
                    log() << "error dropping temp collection: " << ns << endl;
                }
            }
            _tempCollections.clear();
        }
        
        return didAnything;
    }

    BSONObj CurOp::_tooBig = fromjson("{\"$msg\":\"query not recording (too large)\"}");
    AtomicUInt CurOp::_nextOpNum;
    
    Client::Context::Context( string ns , Database * db )
        : _client( currentClient.get() ) , _oldContext( _client->_context ) , 
          _path( dbpath ) , _lock(0) , _justCreated(false) {
        assert( db && db->isOk() );
        _ns = ns;
        _db = db;
        _client->_context = this;
        _auth();
    }

    void Client::Context::_finishInit( bool doauth ){
        int lockState = dbMutex.getState();
        assert( lockState );
        
        _db = dbHolder.get( _ns , _path );
        if ( _db ){
            _justCreated = false;
        }
        else if ( dbMutex.getState() > 0 ){
            // already in a write lock
            _db = dbHolder.getOrCreate( _ns , _path , _justCreated );
            assert( _db );
        }
        else if ( dbMutex.getState() < -1 ){
            // nested read lock :(
            assert( _lock );
            _lock->releaseAndWriteLock();
            _db = dbHolder.getOrCreate( _ns , _path , _justCreated );
            assert( _db );
        }
        else {
            // we have a read lock, but need to get a write lock for a bit
            // we need to be in a write lock since we're going to create the DB object
            // to do that, we're going to unlock, then get a write lock
            // this is so that if this is the first query and its long doesn't block db
            // we just have to check that the db wasn't closed in the interim where we unlock
            for ( int x=0; x<2; x++ ){
                {                     
                    dbtemprelease unlock;
                    writelock lk( _ns );
                    dbHolder.getOrCreate( _ns , _path , _justCreated );
                }
                
                _db = dbHolder.get( _ns , _path );
                
                if ( _db )
                    break;
                
                log() << "db was closed on us right after we opened it: " << _ns << endl;
            }
            
            uassert( 13005 , "can't create db, keeps getting closed" , _db );
        }
        
        _client->_context = this;
        _client->_curOp->enter( this );
        if ( doauth )
            _auth( lockState );
    }

    void Client::Context::_auth( int lockState ){
        if ( _client->_ai.isAuthorizedForLock( _db->name , lockState ) )
            return;

        // before we assert, do a little cleanup
        _client->_context = _oldContext; // note: _oldContext may be null
        
        stringstream ss;
        ss << "unauthorized for db [" << _db->name << "] lock type: " << lockState << endl;
        massert( 10057 , ss.str() , 0 );
    }

    Client::Context::~Context() {
        DEV assert( _client == currentClient.get() );
        _client->_curOp->leave( this );
        _client->_context = _oldContext; // note: _oldContext may be null
    }

    string Client::toString() const {
        stringstream ss;
        if ( _curOp )
            ss << _curOp->infoNoauth().jsonString();
        return ss.str();
    }

    string sayClientState(){
        Client* c = currentClient.get();
        if ( ! c )
            return "no client";
        return c->toString();
    }
    
    void curopWaitingForLock( int type ){
        Client * c = currentClient.get();
        assert( c );
        CurOp * co = c->curop();
        if ( co ){
            co->waitingForLock( type );
        }
    }
    void curopGotLock(){
        Client * c = currentClient.get();
        assert(c);
        CurOp * co = c->curop();
        if ( co ){
            co->gotLock();
        }
    }

    BSONObj CurOp::infoNoauth() {
        BSONObjBuilder b;
        b.append("opid", _opNum);
        bool a = _active && _start;
        b.append("active", a);
        if ( _lockType )
            b.append("lockType" , _lockType > 0 ? "write" : "read"  );
        b.append("waitingForLock" , _waitingForLock );
        
        if( a ){
            b.append("secs_running", elapsedSeconds() );
        }
        
        b.append( "op" , opToString( _op ) );
        
        b.append("ns", _ns);
        
        if( haveQuery() ) {
            b.append("query", query());
        }
        // b.append("inLock",  ??
        stringstream clientStr;
        clientStr << inet_ntoa( _remote.sin_addr ) << ":" << ntohs( _remote.sin_port );
        b.append("client", clientStr.str());

        if ( _client )
            b.append( "desc" , _client->desc() );
        
        if ( ! _message.empty() ){
            if ( _progressMeter.isActive() ){
                StringBuilder buf(128);
                buf << _message << " " << _progressMeter.toString();
                b.append( "msg" , buf.str() );
            }
            else {
                b.append( "msg" , _message );
            }
        }

        return b.obj();
    }

}
