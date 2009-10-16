// security.cpp

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

#include "stdafx.h"
#include "security.h"
#include "instance.h"
#include "client.h"

namespace mongo {

    boost::thread_specific_ptr<Client> currentClient;

    Client::Client() : _database(0), _ns("")/*, _nsstr("")*/ { 
        ai = new AuthenticationInfo(); 
    }

    Client::~Client() { 
        delete ai; 
        ai = 0;
        if ( _tempCollections.size() ){
            cout << "ERROR: Client::shutdown not called!" << endl;
        }
    }

    bool Client::shutdown(){
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
    
    bool noauth = true;

	int AuthenticationInfo::warned;

} // namespace mongo

