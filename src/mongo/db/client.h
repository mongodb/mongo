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
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/

#pragma once

#include "mongo/db/catalog/database.h"
#include "mongo/db/client_basic.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/lasterror.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/concurrency/threadlocal.h"
#include "mongo/util/paths.h"


namespace mongo {

    class AuthenticationInfo;
    class Database;
    class CurOp;
    class Client;
    class Collection;
    class AbstractMessagingPort;


    TSP_DECLARE(Client, currentClient)

    typedef long long ConnectionId;

    /** the database's concept of an outside "client" */
    class Client : public ClientBasic {
    public:
        // always be in clientsMutex when manipulating this. killop stuff uses these.
        static std::set<Client*>& clients;
        static mongo::mutex& clientsMutex;

        ~Client();

        /** each thread which does db operations has a Client object in TLS.
         *  call this when your thread starts.
        */
        static void initThread(const char *desc, AbstractMessagingPort *mp = 0);

        static void initThreadIfNotAlready(const char *desc) { 
            if( currentClient.get() )
                return;
            initThread(desc);
        }

        /** this has to be called as the client goes away, but before thread termination
         *  @return true if anything was done
         */
        bool shutdown();

        std::string clientAddress(bool includePort=false) const;
        CurOp* curop() const { return _curOp; }
        const StringData desc() const { return _desc; }
        void setLastOp( OpTime op ) { _lastOp = op; }
        OpTime getLastOp() const { return _lastOp; }

        /* report what the last operation was.  used by getlasterror */
        void appendLastOp( BSONObjBuilder& b ) const;
        void reportState(BSONObjBuilder& builder);

        // TODO(spencer): SERVER-10228 SERVER-14779 Remove this/move it fully into OperationContext.
        bool isGod() const { return _god; } /* this is for map/reduce writes */
        bool setGod(bool newVal) { const bool prev = _god; _god = newVal; return prev; }

        void setRemoteID(const OID& rid) { _remoteId = rid;  }
        OID getRemoteID() const { return _remoteId; }
        ConnectionId getConnectionId() const { return _connectionId; }
        const std::string& getThreadId() const { return _threadId; }

        // XXX(hk): this is per-thread mmapv1 recovery unit stuff, move into that
        // impl of recovery unit
        void writeHappened() { _hasWrittenSinceCheckpoint = true; }
        bool hasWrittenSinceCheckpoint() const { return _hasWrittenSinceCheckpoint; }
        void checkpointHappened() { _hasWrittenSinceCheckpoint = false; }

        // XXX: this is really a method in the recovery unit iface to reset any state
        void newTopLevelRequest() {
            _hasWrittenSinceCheckpoint = false;
        }

    private:
        Client(const std::string& desc, AbstractMessagingPort *p = 0);
        friend class CurOp;
        ConnectionId _connectionId; // > 0 for things "conn", 0 otherwise
        std::string _threadId; // "" on non support systems
        CurOp * _curOp;
        bool _shutdown; // to track if Client::shutdown() gets called
        std::string _desc;
        bool _god;
        OpTime _lastOp;
        OID _remoteId; // Only used by master-slave

        bool _hasWrittenSinceCheckpoint;
        
    public:

        /* Set database we want to use, then, restores when we finish (are out of scope)
           Note this is also helpful if an exception happens as the state if fixed up.
        */
        class Context {
            MONGO_DISALLOW_COPYING(Context);
        public:
            /** this is probably what you want */
            Context(OperationContext* txn, const std::string& ns, bool doVersion = true);

            /** note: this does not call finishInit -- i.e., does not call 
                      ensureShardVersionOKOrThrow for example.
                see also: reset().
            */
            Context(OperationContext* txn, const std::string& ns, Database * db);

            ~Context();
            Client* getClient() const { return _client; }
            Database* db() const { return _db; }
            const char * ns() const { return _ns.c_str(); }

            /** @return if the db was created by this Context */
            bool justCreated() const { return _justCreated; }

            /** call before unlocking, so clear any non-thread safe state
             *  _db gets restored on the relock
             */
            void unlocked() { _db = 0; }

            /** call after going back into the lock, will re-establish non-thread safe stuff */
            void relocked() { _finishInit(); }

        private:
            friend class CurOp;
            void _finishInit();
            void checkNotStale() const;
            void checkNsAccess( bool doauth );
            void checkNsAccess( bool doauth, int lockState );
            Client * const _client;
            bool _justCreated;
            bool _doVersion;
            const std::string _ns;
            Database * _db;
            OperationContext* _txn;
            
            Timer _timer;
        }; // class Client::Context


        class WriteContext : boost::noncopyable {
        public:
            WriteContext(OperationContext* opCtx, const std::string& ns);

            Database* db() const { return _c.db(); }

            Collection* getCollection() const {
                return _c.db()->getCollection(_txn, _nss.ns());
            }

            Context& ctx() { return _c; }

        private:
            OperationContext* _txn;
            NamespaceString _nss;
            Lock::DBLock _dblk;
            Lock::CollectionLock _collk;
            Context _c;
        };

    }; // class Client


    /**
     * RAII-style class, which acquires a lock on the specified database in the requested mode and
     * obtains a reference to the database. Used as a shortcut for calls to dbHolder().get().
     *
     * It is guaranteed that locks will be released when this object goes out of scope, therefore
     * the database reference returned by this class should not be retained.
     *
     * TODO: This should be moved outside of client.h (maybe dbhelpers.h)
     */
    class AutoGetDb {
        MONGO_DISALLOW_COPYING(AutoGetDb);
    public:
        AutoGetDb(OperationContext* txn, const StringData& ns, LockMode mode);

        Database* getDb() const {
            return _db;
        }

    private:
        const Lock::DBLock _dbLock;
        Database* const _db;
    };

    /**
     * RAII-style class, which would acquire the appropritate hierarchy of locks for obtaining
     * a particular collection and would retrieve a reference to the collection.
     *
     * It is guaranteed that locks will be released when this object goes out of scope, therefore
     * database and collection references returned by this class should not be retained.
     *
     * TODO: This should be moved outside of client.h (maybe dbhelpers.h)
     */
    class AutoGetCollectionForRead {
        MONGO_DISALLOW_COPYING(AutoGetCollectionForRead);
    public:
        AutoGetCollectionForRead(OperationContext* txn, const std::string& ns);
        AutoGetCollectionForRead(OperationContext* txn, const NamespaceString& nss);
        ~AutoGetCollectionForRead();

        Database* getDb() const {
            return _db;
        }

        Collection* getCollection() const {
            return _coll;
        }

    private:
        void _init();

        const Timer _timer;
        OperationContext* const _txn;
        const NamespaceString _nss;
        const Lock::DBLock _dbLock;

        Database* _db;
        Collection* _coll;
    };


    /** get the Client object for this thread. */
    inline Client& cc() {
        Client * c = currentClient.get();
        verify( c );
        return *c;
    }

    inline bool haveClient() { return currentClient.get() != NULL; }

};
