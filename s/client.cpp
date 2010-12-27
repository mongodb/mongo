// s/client.cpp

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

#include "pch.h"
#include "server.h"

#include "../db/commands.h"
#include "../db/dbmessage.h"
#include "../db/stats/counters.h"

#include "../client/connpool.h"

#include "client.h"
#include "request.h"
#include "config.h"
#include "chunk.h"
#include "stats.h"
#include "cursors.h"
#include "grid.h"


namespace mongo {
    
    ClientInfo::ClientInfo( int clientId ) : _id( clientId ){
        _cur = &_a;
        _prev = &_b;
        newRequest();
    }
    
    ClientInfo::~ClientInfo(){
        if ( _lastAccess ){
            scoped_lock lk( _clientsLock );
            Cache::iterator i = _clients.find( _id );
            if ( i != _clients.end() ){
                _clients.erase( i );
            }
        }
    }
    
    void ClientInfo::addShard( const string& shard ){
        _cur->insert( shard );
        _sinceLastGetError.insert( shard );
    }
    
    void ClientInfo::newRequest( AbstractMessagingPort* p ){

        if ( p ){
            string r = p->remote().toString();
            if ( _remote == "" )
                _remote = r;
            else if ( _remote != r ){
                stringstream ss;
                ss << "remotes don't match old [" << _remote << "] new [" << r << "]";
                throw UserException( 13134 , ss.str() );
            }
        }
        
        _lastAccess = (int) time(0);
        
        set<string> * temp = _cur;
        _cur = _prev;
        _prev = temp;
        _cur->clear();
    }
    
    void ClientInfo::disconnect(){
        _lastAccess = 0;
    }
        
    ClientInfo * ClientInfo::get( int clientId , bool create ){
        
        if ( ! clientId )
            clientId = getClientId();
        
        if ( ! clientId ){
            ClientInfo * info = _tlInfo.get();
            if ( ! info ){
                info = new ClientInfo( 0 );
                _tlInfo.reset( info );
            }
            info->newRequest();
            return info;
        }
        
        scoped_lock lk( _clientsLock );
        Cache::iterator i = _clients.find( clientId );
        if ( i != _clients.end() )
            return i->second;
        if ( ! create )
            return 0;
        ClientInfo * info = new ClientInfo( clientId );
        _clients[clientId] = info;
        return info;
    }
        
    void ClientInfo::disconnect( int clientId ){
        if ( ! clientId )
            return;

        scoped_lock lk( _clientsLock );
        Cache::iterator i = _clients.find( clientId );
        if ( i == _clients.end() )
            return;

        ClientInfo* ci = i->second;
        ci->disconnect();
        delete ci;
        _clients.erase( i );
    }

    ClientInfo::Cache& ClientInfo::_clients = *(new ClientInfo::Cache());
    mongo::mutex ClientInfo::_clientsLock("_clientsLock");
    boost::thread_specific_ptr<ClientInfo> ClientInfo::_tlInfo;

} // namespace mongo
