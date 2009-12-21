// client.h

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

/* Client represents a connection to the database (the server-side) and corresponds 
   to an open socket (or logical connection if pooling on sockets) from a client.

   todo: switch to asio...this will fit nicely with that.
*/

#pragma once

#include "../stdafx.h"
#include "namespace.h"
#include "lasterror.h"
#include "../util/top.h"

namespace mongo { 

    class AuthenticationInfo;
    class Database;
    class CurOp;

    class Client : boost::noncopyable { 
    public:
        static boost::mutex clientsMutex;
        static set<Client*> clients; // always be in clientsMutex when manipulating this

        class GodScope {
            bool _prev;
        public:
            GodScope();
            ~GodScope();
        };

    private:
        CurOp * const _curOp;
        Database *_database;
        Namespace _ns;
        //NamespaceString _nsstr;
        bool _shutdown;
        list<string> _tempCollections;
        const char *_desc;
        bool _god;
    public:
        AuthenticationInfo *ai;
        Top top;

        CurOp* curop() { return _curOp; }
        Database* database() { 
            return _database; 
        }
        const char *ns() { return _ns.buf; }

        void setns(const char *ns, Database *db) { 
            _database = db;
            _ns = ns;
            //_nsstr = ns;
        }
        void clearns() { setns("", 0); }

        Client(const char *desc);
        ~Client();

        const char *desc() const { return _desc; }

        void addTempCollection( const string& ns ){
            _tempCollections.push_back( ns );
        }

        /* each thread which does db operations has a Client object in TLS.  
           call this when your thread starts. 
        */
        static void initThread(const char *desc);

        /* 
           this has to be called as the client goes away, but before thread termination
           @return true if anything was done
         */
        bool shutdown();

        bool isGod() const { return _god; }
    };
    
    /* defined in security.cpp - one day add client.cpp? */
    extern boost::thread_specific_ptr<Client> currentClient;

    inline Client& cc() { 
        return *currentClient.get();
    }

    /* each thread which does db operations has a Client object in TLS.  
       call this when your thread starts. 
    */
    inline void Client::initThread(const char *desc) {
        assert( currentClient.get() == 0 );
        currentClient.reset( new Client(desc) );
    }

    inline Client::GodScope::GodScope(){
        _prev = cc()._god;
        cc()._god = true;
    }

    inline Client::GodScope::~GodScope(){
        cc()._god = _prev;
    }

	/* this unlocks, does NOT upgrade. that works for our current usage */
    inline void mongolock::releaseAndWriteLock() { 
        if( !_writelock ) {

#if BOOST_VERSION >= 103500
            int s = dbMutex.getState();
            if( s != -1 ) {
                log() << "error: releaseAndWriteLock() s == " << s << endl;
                msgasserted( "releaseAndWriteLock: unlock_shared failed, probably recursive" );
            }
#endif

            _writelock = true;
            dbMutex.unlock_shared();
            dbMutex.lock();

            /* this is defensive; as we were unlocked for a moment above, 
               the Database object we reference could have been deleted:
            */
            cc().clearns();
        }
    }
    
};

