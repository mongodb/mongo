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

#include "../pch.h"
#include "security.h"
#include "namespace-inl.h"
#include "lasterror.h"
#include "stats/top.h"
#include "../util/concurrency/threadlocal.h"
#include "../db/client_common.h"
#include "../util/net/message_port.h"

namespace mongo {

    extern class ReplSet *theReplSet;
    class AuthenticationInfo;
    class Database;
    class CurOp;
    class Command;
    class Client;
    class AbstractMessagingPort;

    TSP_DECLARE(Client, currentClient)

    typedef long long ConnectionId;

    /** the database's concept of an outside "client" */
    class Client : public ClientBasic {
    public:
        class Context;

        static mongo::mutex clientsMutex;
        static set<Client*> clients; // always be in clientsMutex when manipulating this
        static int recommendedYieldMicros( int * writers = 0 , int * readers = 0 );
        static int getActiveClientCount( int& writers , int& readers );
        static Client *syncThread;

        /* each thread which does db operations has a Client object in TLS.
           call this when your thread starts.
        */
        static Client& initThread(const char *desc, AbstractMessagingPort *mp = 0);

        static void initThreadIfNotAlready(const char *desc) { 
            if( currentClient.get() )
                return;
            initThread(desc);
        }

        ~Client();

        /*
           this has to be called as the client goes away, but before thread termination
           @return true if anything was done
         */
        bool shutdown();

        /**  set so isSyncThread() works */
        void iAmSyncThread() {
            wassert( syncThread == 0 );
            syncThread = this;
        }
        /** @return true if this client is the replication secondary pull thread.  not used much, is used in create index sync code. */
        bool isSyncThread() const { return this == syncThread; }

        string clientAddress(bool includePort=false) const;
        const AuthenticationInfo * getAuthenticationInfo() const { return &_ai; }
        AuthenticationInfo * getAuthenticationInfo() { return &_ai; }
        bool isAdmin() { return _ai.isAuthorized( "admin" ); }
        CurOp* curop() const { return _curOp; }
        Context* getContext() const { return _context; }
        Database* database() const {  return _context ? _context->db() : 0; }
        const char *ns() const { return _context->ns(); }
        const char *desc() const { return _desc; }
        void setLastOp( OpTime op ) { _lastOp = op; }
        OpTime getLastOp() const { return _lastOp; }

        /** caution -- use Context class instead */
        void setContext(Context *c) { _context = c; }

        /* report what the last operation was.  used by getlasterror */
        void appendLastOp( BSONObjBuilder& b ) const;

        bool isGod() const { return _god; } /* this is for map/reduce writes */
        string toString() const;
        void gotHandshake( const BSONObj& o );
        bool hasRemote() const { return _mp; }
        HostAndPort getRemote() const { assert( _mp ); return _mp->remote(); }
        BSONObj getRemoteID() const { return _remoteId; }
        BSONObj getHandshake() const { return _handshake; }
        AbstractMessagingPort * port() const { return _mp; }
        ConnectionId getConnectionId() const { return _connectionId; }

    private:
        ConnectionId _connectionId; // > 0 for things "conn", 0 otherwise
        string _threadId; // "" on non support systems
        CurOp * _curOp;
        Context * _context;
        bool _shutdown;
        const char *_desc;
        bool _god;
        AuthenticationInfo _ai;
        OpTime _lastOp;
        BSONObj _handshake;
        BSONObj _remoteId;
        AbstractMessagingPort * const _mp;

        Client(const char *desc, AbstractMessagingPort *p = 0);

        friend class CurOp;

        unsigned _sometimes;

    public:
        /** the concept here is the same as MONGO_SOMETIMES.  however that 
            macro uses a static that will be shared by all threads, and each 
            time incremented it might eject that line from the other cpu caches (?),
            so idea is that this is better.
            */
        bool sometimes(unsigned howOften) { return ++_sometimes % howOften == 0; }

    public:

        /* set _god=true temporarily, safely */
        class GodScope {
            bool _prev;
        public:
            GodScope();
            ~GodScope();
        };

        /* Set database we want to use, then, restores when we finish (are out of scope)
           Note this is also helpful if an exception happens as the state if fixed up.
        */
        class Context : boost::noncopyable {
        public:
            /**
             * this is the main constructor
             * use this unless there is a good reason not to
             */
            Context(const string& ns, string path=dbpath, mongolock * lock = 0 , bool doauth=true );

            /* this version saves the context but doesn't yet set the new one: */
            Context();

            /**
             * if you are doing this after allowing a write there could be a race condition
             * if someone closes that db.  this checks that the DB is still valid
             */
            Context( string ns , Database * db, bool doauth=true );

            ~Context();

            Client* getClient() const { return _client; }
            Database* db() const { return _db; }
            const char * ns() const { return _ns.c_str(); }

            /** @return if the db was created by this Context */
            bool justCreated() const { return _justCreated; }

            bool equals( const string& ns , const string& path=dbpath ) const { return _ns == ns && _path == path; }

            /**
             * @return true iff the current Context is using db/path
             */
            bool inDB( const string& db , const string& path=dbpath ) const;

            void clear() { _ns = ""; _db = 0; }

            /**
             * call before unlocking, so clear any non-thread safe state
             */
            void unlocked() { _db = 0; }

            /**
             * call after going back into the lock, will re-establish non-thread safe stuff
             */
            void relocked() { _finishInit(); }

            friend class CurOp;

        private:
            /**
             * at this point _client, _oldContext and _ns have to be set
             * _db should not have been touched
             * this will set _db and create if needed
             * will also set _client->_context to this
             */
            void _finishInit( bool doauth=true);

            void _auth( int lockState = dbMutex.getState() );

            Client * _client;
            Context * _oldContext;

            string _path;
            mongolock * _lock;
            bool _justCreated;

            string _ns;
            Database * _db;

        }; // class Client::Context

    }; // class Client

    /** get the Client object for this thread. */
    inline Client& cc() {
        Client * c = currentClient.get();
        assert( c );
        return *c;
    }

    inline Client::GodScope::GodScope() {
        _prev = cc()._god;
        cc()._god = true;
    }

    inline Client::GodScope::~GodScope() { cc()._god = _prev; }

    /* this unreadlocks and then writelocks; i.e. it does NOT upgrade inside the
       lock (and is thus wrong to use if you need that, which is usually).
       that said we use it today for a specific case where the usage is correct.
    */
    inline void mongolock::releaseAndWriteLock() {
        if( !_writelock ) {

#if BOOST_VERSION >= 103500
            int s = dbMutex.getState();
            if( s != -1 ) {
                log() << "error: releaseAndWriteLock() s == " << s << endl;
                msgasserted( 12600, "releaseAndWriteLock: unlock_shared failed, probably recursive" );
            }
#endif

            _writelock = true;
            dbMutex.unlock_shared();
            dbMutex.lock();

            // todo: unlocked() method says to call it before unlocking, not after.  so fix this here,
            // or fix the doc there.
            if ( cc().getContext() )
                cc().getContext()->unlocked();
        }
    }

    string sayClientState();

    inline bool haveClient() { return currentClient.get() > 0; }
};
