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

#include "../pch.h"
#include "../util/message.h"
#include "../db/dbmessage.h"
#include "config.h"
#include "util.h"

namespace mongo {

    
    class OpCounters;
    class ClientInfo;
    
    class Request : boost::noncopyable {
    public:
        Request( Message& m, AbstractMessagingPort* p );

        // ---- message info -----
        

        const char * getns() const {
            return _d.getns();
        }
        int op() const {
            return _m.operation();
        }
        bool expectResponse() const {
            return op() == dbQuery || op() == dbGetMore;
        }
        bool isCommand() const;

        MSGID id() const {
            return _id;
        }

        DBConfigPtr getConfig() const {
            assert( _didInit );
            return _config;
        }
        bool isShardingEnabled() const {
            assert( _didInit );
            return _config->isShardingEnabled();
        }
        
        ChunkManagerPtr getChunkManager() const {
            assert( _didInit );
            return _chunkManager;
        }
        
        int getClientId() const {
            return _clientId;
        }
        ClientInfo * getClientInfo() const {
            return _clientInfo;
        }

        // ---- remote location info -----
        
        
        Shard primaryShard() const ;
        
        // ---- low level access ----

        void reply( Message & response , const string& fromServer );
        
        Message& m() { return _m; }
        DbMessage& d() { return _d; }
        AbstractMessagingPort* p() const { return _p; }

        void process( int attempt = 0 );

        void gotInsert();

        void init();

        void reset( bool reload=false );
        
    private:
        Message& _m;
        DbMessage _d;
        AbstractMessagingPort* _p;
        
        MSGID _id;
        DBConfigPtr _config;
        ChunkManagerPtr _chunkManager;
        
        int _clientId;
        ClientInfo * _clientInfo;

        OpCounters* _counter;

        bool _didInit;
    };

    typedef map<int,ClientInfo*> ClientCache;
    
    class ClientInfo {
    public:
        ClientInfo( int clientId );
        ~ClientInfo();
        
        string getRemote() const { return _remote; }

        void addShard( const string& shard );
        set<string> * getPrev() const { return _prev; };
        
        void newRequest( AbstractMessagingPort* p = 0 );
        void disconnect();
        
        static ClientInfo * get( int clientId = 0 , bool create = true );
        static void disconnect( int clientId );
        
        const set<string>& sinceLastGetError() const { return _sinceLastGetError; }
        void clearSinceLastGetError(){ 
            _sinceLastGetError.clear(); 
        }

    private:
        int _id;
        string _remote;

        set<string> _a;
        set<string> _b;
        set<string> * _cur;
        set<string> * _prev;
        int _lastAccess;
        
        set<string> _sinceLastGetError;

        static mongo::mutex _clientsLock;
        static ClientCache& _clients;
        static boost::thread_specific_ptr<ClientInfo> _tlInfo;
    };
}

#include "strategy.h"
