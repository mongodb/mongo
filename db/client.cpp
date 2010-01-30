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
 
namespace mongo {

    boost::mutex Client::clientsMutex;
    set<Client*> Client::clients; // always be in clientsMutex when manipulating this
    boost::thread_specific_ptr<Client> currentClient;

    Client::Client(const char *desc) : 
      _curOp(new CurOp()),
      _context(0),
      //_database(0), _ns("")/*, _nsstr("")*/ 
      _shutdown(false),
      _desc(desc),
      _god(0),
      _prevDB( 0 )
    {
        ai = new AuthenticationInfo(); 
        boostlock bl(clientsMutex);
        clients.insert(this);
    }

    Client::~Client() { 
        delete _curOp;
        delete ai; 
        ai = 0;
        _god = 0;

        if ( _context )
            cout << "ERROR: Client::~Client _context should be NULL" << endl;
        if ( !_shutdown ) 
            cout << "ERROR: Client::shutdown not called!" << endl;
    }

    bool Client::shutdown(){
        _shutdown = true;

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
    }

    void Client::Context::_finishInit(){
        dbMutex.assertAtLeastReadLocked();
        
        _db = dbHolder.get( _ns , _path );
        if ( _db ){
            _justCreated = false;
        }
        else {
            // we need to be in a write lock since we're going to create the DB object
            if ( _lock )
                _lock->releaseAndWriteLock();
            assertInWriteLock();
            
            _db = dbHolder.getOrCreate( _ns , _path , _justCreated );
        }
        
        _client->_context = this;
    }

}
