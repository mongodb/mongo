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
#include "namespace.h"
#include "lasterror.h"
#include "stats/top.h"

namespace mongo { 

    extern class ReplSet *theReplSet;
    class AuthenticationInfo;
    class Database;
    class CurOp;
    class Command;
    class Client;

    extern boost::thread_specific_ptr<Client> currentClient;

    class Client : boost::noncopyable { 
    public:
        static Client *syncThread;
        void iAmSyncThread() { 
            wassert( syncThread == 0 );
            syncThread = this; 
        }
        bool isSyncThread() const { return this == syncThread; } // true if this client is the replication secondary pull thread

        static mongo::mutex clientsMutex;
        static set<Client*> clients; // always be in clientsMutex when manipulating this

        static int recommendedYieldMicros( int * writers = 0 , int * readers = 0 );

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
        class Context : boost::noncopyable{
            Client * _client;
            Context * _oldContext;
            
            string _path;
            mongolock * _lock;
            bool _justCreated;

            string _ns;
            Database * _db;

            /**
             * at this point _client, _oldContext and _ns have to be set
             * _db should not have been touched
             * this will set _db and create if needed
             * will also set _client->_context to this
             */
            void _finishInit( bool doauth=true);
            
            void _auth( int lockState = dbMutex.getState() );
        public:
            Context(const string& ns, string path=dbpath, mongolock * lock = 0 , bool doauth=true ) 
                : _client( currentClient.get() ) , _oldContext( _client->_context ) , 
                  _path( path ) , _lock( lock ) , 
                  _ns( ns ), _db(0){
                _finishInit( doauth );
            }
            
            /* this version saves the context but doesn't yet set the new one: */
            
            Context() 
                : _client( currentClient.get() ) , _oldContext( _client->_context ), 
                  _path( dbpath ) , _lock(0) , _justCreated(false), _db(0){
                _client->_context = this;
                clear();
            }
            
            /**
             * if you are doing this after allowing a write there could be a race condition
             * if someone closes that db.  this checks that the DB is still valid
             */
            Context( string ns , Database * db, bool doauth=true );
            
            ~Context();

            Client* getClient() const { return _client; }            
            Database* db() const { return _db; }
            const char * ns() const { return _ns.c_str(); }            
            bool justCreated() const { return _justCreated; }

            bool equals( const string& ns , const string& path=dbpath ) const {
                return _ns == ns && _path == path;
            }

            bool inDB( const string& db , const string& path=dbpath ) const {
                if ( _path != path )
                    return false;
                
                if ( db == _ns )
                    return true;

                string::size_type idx = _ns.find( db );
                if ( idx != 0 )
                    return false;
                
                return  _ns[db.size()] == '.';
            }

            void clear(){
                _ns = "";
                _db = 0;
            }

            /**
             * call before unlocking, so clear any non-thread safe state
             */
            void unlocked(){
                _db = 0;
            }

            /**
             * call after going back into the lock, will re-establish non-thread safe stuff
             */
            void relocked(){
                _finishInit();
            }

            friend class CurOp;
        };
        
    private:
        CurOp * _curOp;
        Context * _context;
        bool _shutdown;
        set<string> _tempCollections;
        const char *_desc;
        bool _god;
        AuthenticationInfo _ai;
        ReplTime _lastOp;
        BSONObj _handshake;
        BSONObj _remoteId;

        void _dropns( const string& ns );

    public:
        string clientAddress() const;
        AuthenticationInfo * getAuthenticationInfo(){ return &_ai; }
        bool isAdmin() { return _ai.isAuthorized( "admin" ); }
        CurOp* curop() { return _curOp; }        
        Context* getContext(){ return _context; }
        Database* database() {  return _context ? _context->db() : 0; }
        const char *ns() const { return _context->ns(); }
        const char *desc() const { return _desc; }
        
        Client(const char *desc);
        ~Client();

        void addTempCollection( const string& ns );
        
        void _invalidateDB(const string& db);
        static void invalidateDB(const string& db);
        static void invalidateNS( const string& ns );

        void setLastOp( ReplTime op ) { _lastOp = op; }
        ReplTime getLastOp() const { return _lastOp; }

        /* report what the last operation was.  used by getlasterror */
        void appendLastOp( BSONObjBuilder& b ) {
            if( theReplSet ) { 
                b.append("lastOp" , (long long) _lastOp);
            }
            else {
                OpTime lo(_lastOp);
                if ( ! lo.isNull() )
                    b.appendTimestamp( "lastOp" , lo.asDate() );
            }
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
        
        /* this is for map/reduce writes */
        bool isGod() const { return _god; }

        friend class CurOp;

        string toString() const;
        void gotHandshake( const BSONObj& o );
        BSONObj getRemoteID() const { return _remoteId; }
        BSONObj getHandshake() const { return _handshake; }
    };
    
    /** get the Client object for this thread. */
    inline Client& cc() { 
        Client * c = currentClient.get();
        assert( c );
        return *c;
    }

    /* each thread which does db operations has a Client object in TLS.  
       call this when your thread starts. 
    */
    inline void Client::initThread(const char *desc) {
        setThreadName(desc);
        assert( currentClient.get() == 0 );
        currentClient.reset( new Client(desc) );
        mongo::lastError.initThread();
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
                msgasserted( 12600, "releaseAndWriteLock: unlock_shared failed, probably recursive" );
            }
#endif

            _writelock = true;
            dbMutex.unlock_shared();
            dbMutex.lock();

            if ( cc().getContext() )
                cc().getContext()->unlocked();
        }
    }

    string sayClientState();
  
    inline bool haveClient() { return currentClient.get() > 0; }
};
