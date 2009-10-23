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
 
namespace mongo {

    boost::mutex Client::clientsMutex;
    set<Client*> Client::clients; // always be in clientsMutex when manipulating this
    boost::thread_specific_ptr<Client> currentClient;

    Client::Client(const char *desc) : 
      _op(new CurOp()),
      _database(0), _ns("")/*, _nsstr("")*/ 
      ,_shutdown(false),
      _desc(desc),
      _god(0)
    { 
        ai = new AuthenticationInfo(); 

        boostlock bl(clientsMutex);
        clients.insert(this);
    }

    Client::~Client() { 
        delete _op;
        delete ai; 
        ai = 0;
        _god = 0;
        if ( !_shutdown ) {
            cout << "ERROR: Client::shutdown not called!" << endl;
        }
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
                setClient( ns.c_str() );
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

}
