// request.h
/*
 *    Copyright (C) 2010 10gen Inc.
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


#pragma once

#include "../stdafx.h"
#include "../util/message.h"
#include "../db/dbmessage.h"
#include "config.h"
#include "util.h"

namespace mongo {
    
    class ClientInfo;
    
    class Request : boost::noncopyable {
    public:
        Request( Message& m, AbstractMessagingPort* p );

        // ---- message info -----


        const char * getns(){
            return _d.getns();
        }
        int op(){
            return _m.data->operation();
        }
        bool expectResponse(){
            return op() == dbQuery || op() == dbGetMore;
        }
        
        MSGID id(){
            return _id;
        }

        DBConfig * getConfig(){
            return _config;
        }
        bool isShardingEnabled(){
            return _config->isShardingEnabled();
        }
        
        ChunkManager * getChunkManager(){
            return _chunkManager;
        }
        
        int getClientId(){
            return _clientId;
        }
        ClientInfo * getClientInfo(){
            return _clientInfo;
        }

        // ---- remote location info -----

        
        string singleServerName();
        
        const char * primaryName(){
            return _config->getPrimary().c_str();
        }

        // ---- low level access ----

        void reply( Message & response ){
            _p->reply( _m , response , _id );
        }
        
        Message& m(){ return _m; }
        DbMessage& d(){ return _d; }
        AbstractMessagingPort* p(){ return _p; }

        void process( int attempt = 0 );
        
    private:
        
        void reset( bool reload=false );
        
        Message& _m;
        DbMessage _d;
        AbstractMessagingPort* _p;
        
        MSGID _id;
        DBConfig * _config;
        ChunkManager * _chunkManager;
        
        int _clientId;
        ClientInfo * _clientInfo;
    };

    typedef map<int,ClientInfo*> ClientCache;
    
    class ClientInfo {
    public:
        ClientInfo( int clientId );
        ~ClientInfo();
        
        void addShard( const string& shard );
        set<string> * getPrev() const { return _prev; };
        
        void newRequest();
        void disconnect();

        static ClientInfo * get( int clientId = 0 , bool create = true );
        
    private:
        int _id;
        set<string> _a;
        set<string> _b;
        set<string> * _cur;
        set<string> * _prev;
        int _lastAccess;
        
        static mongo::mutex _clientsLock;
        static ClientCache _clients;
        static boost::thread_specific_ptr<ClientInfo> _tlInfo;
    };
}

#include "strategy.h"
