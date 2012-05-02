/* @file db/client.h

   "Client" represents a connection to the database (the server-side) and corresponds
   to an open socket (or logical connection if pooling on sockets) from a client.

   todo: switch to asio...this will fit nicely with that.
*/

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

#pragma once

#include "../pch.h"
#include "security.h"
#include "namespace-inl.h"
#include "lasterror.h"
#include "stats/top.h"
#include "../db/client_common.h"
#include "../util/concurrency/threadlocal.h"
#include "../util/net/message_port.h"
#include "../util/concurrency/rwlock.h"
#include "d_concurrency.h"
#include "mongo/util/paths.h"

namespace mongo {

    extern class ReplSet *theReplSet;
    class AuthenticationInfo;
    class Database;
    class CurOp;
    class Command;
    class Client;
    class AbstractMessagingPort;
    class LockCollectionForReading;
    class PageFaultRetryableSection;

    TSP_DECLARE(Client, currentClient)

    typedef long long ConnectionId;

    /** the database's concept of an outside "client" */
    class Client : public ClientBasic {
        static Client *syncThread;
    public:
        LockState _ls;

        // always be in clientsMutex when manipulating this. killop stuff uses these.
        static set<Client*>& clients;
        static mongo::mutex& clientsMutex;
        static int getActiveClientCount( int& writers , int& readers );
        class Context;
        ~Client();
        static int recommendedYieldMicros( int * writers = 0 , int * readers = 0 );

        /** each thread which does db operations has a Client object in TLS.
         *  call this when your thread starts.
        */
        static Client& initThread(const char *desc, AbstractMessagingPort *mp = 0);

        static void initThreadIfNotAlready(const char *desc) { 
            if( currentClient.get() )
                return;
            initThread(desc);
        }

        /** this has to be called as the client goes away, but before thread termination
         *  @return true if anything was done
         */
        bool shutdown();

        /** set so isSyncThread() works */
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
        const std::string desc() const { return _desc; }
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
        HostAndPort getRemote() const { verify( _mp ); return _mp->remote(); }
        BSONObj getRemoteID() const { return _remoteId; }
        BSONObj getHandshake() const { return _handshake; }
        AbstractMessagingPort * port() const { return _mp; }
        ConnectionId getConnectionId() const { return _connectionId; }

        bool inPageFaultRetryableSection() const { return _pageFaultRetryableSection != 0; }
        PageFaultRetryableSection* getPageFaultRetryableSection() const { return _pageFaultRetryableSection; }
        
        bool hasWrittenThisPass() const { return _hasWrittenThisPass; }
        void writeHappened() { _hasWrittenThisPass = true; }
        void newTopLevelRequest() { _hasWrittenThisPass = false; }
        
        bool allowedToThrowPageFaultException() const;

    private:
        Client(const char *desc, AbstractMessagingPort *p = 0);
        friend class CurOp;
        ConnectionId _connectionId; // > 0 for things "conn", 0 otherwise
        string _threadId; // "" on non support systems
        CurOp * _curOp;
        Context * _context;
        bool _shutdown; // to track if Client::shutdown() gets called
        const std::string _desc;
        bool _god;
        AuthenticationInfo _ai;
        OpTime _lastOp;
        BSONObj _handshake;
        BSONObj _remoteId;
        AbstractMessagingPort * const _mp;
        unsigned _sometimes;

        bool _hasWrittenThisPass;
        PageFaultRetryableSection *_pageFaultRetryableSection;
        
        friend class PageFaultRetryableSection; // TEMP
    public:

        /** the concept here is the same as MONGO_SOMETIMES.  however that 
            macro uses a static that will be shared by all threads, and each 
            time incremented it might eject that line from the other cpu caches (?),
            so idea is that this is better.
            */
        bool sometimes(unsigned howOften) { return ++_sometimes % howOften == 0; }

        /* set _god=true temporarily, safely */
        class GodScope {
            bool _prev;
        public:
            GodScope();
            ~GodScope();
        };

        //static void assureDatabaseIsOpen(const string& ns, string path=dbpath);
        
        /** "read lock, and set my context, all in one operation" 
         *  This handles (if not recursively locked) opening an unopened database.
         */
        class ReadContext : boost::noncopyable { 
        public:
            ReadContext(const string& ns, string path=dbpath, bool doauth=true );
            Context& ctx() { return *c.get(); }
        private:
            scoped_ptr<Lock::DBRead> lk;
            scoped_ptr<Context> c;
        };

        /* Set database we want to use, then, restores when we finish (are out of scope)
           Note this is also helpful if an exception happens as the state if fixed up.
        */
        class Context : boost::noncopyable {
        public:
            /** this is probably what you want */
            Context(const string& ns, string path=dbpath, bool doauth=true, bool doVersion=true );

            /** note: this does not call finishInit -- i.e., does not call 
                      shardVersionOk() for example. 
                see also: reset().
            */
            Context( string ns , Database * db, bool doauth=true );

            // used by ReadContext
            Context(const string& path, const string& ns, Database *db, bool doauth);

            ~Context();
            Client* getClient() const { return _client; }
            Database* db() const { return _db; }
            const char * ns() const { return _ns.c_str(); }
            bool equals( const string& ns , const string& path=dbpath ) const { return _ns == ns && _path == path; }

            /** @return if the db was created by this Context */
            bool justCreated() const { return _justCreated; }

            /** @return true iff the current Context is using db/path */
            bool inDB( const string& db , const string& path=dbpath ) const;

            void _clear() { // this is sort of an "early destruct" indication, _ns can never be uncleared
                const_cast<string&>(_ns).clear();
                _db = 0;
            }

            /** call before unlocking, so clear any non-thread safe state
             *  _db gets restored on the relock
             */
            void unlocked() { _db = 0; }

            /** call after going back into the lock, will re-establish non-thread safe stuff */
            void relocked() { _finishInit(); }

        private:
            friend class CurOp;
            void _finishInit( bool doauth=true);
            void _auth( int lockState );
            void checkNotStale() const;
            void checkNsAccess( bool doauth );
            void checkNsAccess( bool doauth, int lockState );
            Client * const _client;
            Context * const _oldContext;
            const string _path;
            bool _justCreated;
            bool _doVersion;
            const string _ns;
            Database * _db;
        }; // class Client::Context

        class WriteContext : boost::noncopyable {
        public:
            WriteContext(const string& ns, string path=dbpath, bool doauth=true );
            Context& ctx() { return _c; }
        private:
            Lock::DBWrite _lk;
            Context _c;
        };


    }; // class Client


    /** get the Client object for this thread. */
    inline Client& cc() {
        Client * c = currentClient.get();
        verify( c );
        return *c;
    }

    inline Client::GodScope::GodScope() {
        _prev = cc()._god;
        cc()._god = true;
    }
    inline Client::GodScope::~GodScope() { cc()._god = _prev; }


    inline bool haveClient() { return currentClient.get() > 0; }

};
