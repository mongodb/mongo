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

#include <boost/noncopyable.hpp>
#include <boost/scoped_ptr.hpp>
#include <boost/thread/thread.hpp>

#include "mongo/db/catalog/database.h"
#include "mongo/db/client_basic.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/lasterror.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/platform/unordered_set.h"
#include "mongo/stdx/functional.h"
#include "mongo/util/concurrency/spin_lock.h"
#include "mongo/util/concurrency/threadlocal.h"
#include "mongo/util/paths.h"

namespace mongo {

    class AuthenticationInfo;
    class CurOp;
    class Collection;
    class AbstractMessagingPort;
    class Locker;

    TSP_DECLARE(Client, currentClient)

    typedef long long ConnectionId;

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
     * RAII-style class, which acquires a lock on the specified database in the requested mode and
     * obtains a reference to the database, creating it was non-existing. Used as a shortcut for
     * calls to dbHolder().openDb(), taking care of locking details. The requested mode must be
     * MODE_IX or MODE_X. If the database needs to be created, the lock will automatically be
     * reacquired as MODE_X.
     *
     * It is guaranteed that locks will be released when this object goes out of scope, therefore
     * the database reference returned by this class should not be retained.
     *
     * TODO: This should be moved outside of client.h (maybe dbhelpers.h)
     */
    class AutoGetOrCreateDb {
        MONGO_DISALLOW_COPYING(AutoGetOrCreateDb);
    public:
        AutoGetOrCreateDb(OperationContext* txn, const StringData& ns, LockMode mode);

        Database* getDb() {
            return _db;
        }

        bool justCreated() {
            return _justCreated;
        }

        Lock::DBLock& lock() { return _dbLock; }

    private:
        ScopedTransaction _transaction;
        Lock::DBLock _dbLock; // not const, as we may need to relock for implicit create
        Database* _db;
        bool _justCreated;
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
            return _db.getDb();
        }

        Collection* getCollection() const {
            return _coll;
        }

    private:
        void _init(const std::string& ns,
                   const StringData& coll);

        const Timer _timer;
        OperationContext* const _txn;
        const ScopedTransaction _transaction;
        const AutoGetDb _db;
        const Lock::CollectionLock _collLock;

        Collection* _coll;
    };

    typedef unordered_set<Client*> ClientSet;

    /** the database's concept of an outside "client" */
    class Client : public ClientBasic {
    public:
        // A set of currently active clients along with a mutex to protect the list
        static boost::mutex clientsMutex;
        static ClientSet clients;

        ~Client();

        /** each thread which does db operations has a Client object in TLS.
         *  call this when your thread starts.
        */
        static void initThread(const char *desc, AbstractMessagingPort *mp = 0);

        /**
         * Inits a thread if that thread has not already been init'd, setting the thread name to
         * "desc".
         */
        static void initThreadIfNotAlready(const char *desc) { 
            if (currentClient.get())
                return;
            initThread(desc);
        }

        /**
         * Inits a thread if that thread has not already been init'd, using the existing thread name
         */
        static void initThreadIfNotAlready() {
            if (currentClient.get())
                return;
            initThread(getThreadName().c_str());
        }

        /** this has to be called as the client goes away, but before thread termination
         *  @return true if anything was done
         */
        bool shutdown();

        std::string clientAddress(bool includePort=false) const;
        CurOp* curop() const { return _curOp; }
        const std::string& desc() const { return _desc; }
        void setLastOp( OpTime op ) { _lastOp = op; }
        OpTime getLastOp() const { return _lastOp; }

        // Return a reference to the Locker for this client. Client retains ownership.
        Locker* getLocker() const { return _locker.get(); }

        /* report what the last operation was.  used by getlasterror */
        void appendLastOp( BSONObjBuilder& b ) const;
        void reportState(BSONObjBuilder& builder);

        // Ensures stability of the client's OperationContext. When the client is locked,
        // the OperationContext will not disappear.
        void lock() { _lock.lock(); }
        void unlock() { _lock.unlock(); }

        // Changes the currently active operation context on this client. There can only be one
        // active OperationContext at a time.
        void setOperationContext(OperationContext* txn);
        void resetOperationContext();
        const OperationContext* getOperationContext() const { return _txn; }

        // TODO(spencer): SERVER-10228 SERVER-14779 Remove this/move it fully into OperationContext.
        bool isGod() const { return _god; } /* this is for map/reduce writes */
        bool setGod(bool newVal) { const bool prev = _god; _god = newVal; return prev; }

        void setRemoteID(const OID& rid) { _remoteId = rid;  } // Only used for master/slave
        OID getRemoteID() const { return _remoteId; } // Only used for master/slave
        ConnectionId getConnectionId() const { return _connectionId; }
        bool isFromUserConnection() const { return _connectionId > 0; }

    private:
        Client(const std::string& desc, AbstractMessagingPort *p = 0);
        friend class CurOp;

        // Description for the client (e.g. conn8)
        const std::string _desc;

        // OS id of the thread, which owns this client
        const boost::thread::id _threadId;

        // > 0 for things "conn", 0 otherwise
        const ConnectionId _connectionId;

        // Protects the contents of the Client (such as changing the OperationContext, etc)
        mutable SpinLock _lock;

        // Whether this client is running as DBDirectClient
        bool _god;

        // If != NULL, then contains the currently active OperationContext
        OperationContext* _txn;

        // Changes, based on what operation is running. Some of this should be in OperationContext.
        CurOp* _curOp;

        // By having Client, rather than the OperationContext, own the Locker,  setup cost such as
        // allocating OS resources can be amortized over multiple operations.
        boost::scoped_ptr<Locker> const _locker;

        // Used by replication
        OpTime _lastOp;
        OID _remoteId; // Only used by master-slave

        // Tracks if Client::shutdown() gets called (TODO: Is this necessary?)
        bool _shutdown;

    public:

        /* Set database we want to use, then, restores when we finish (are out of scope)
           Note this is also helpful if an exception happens as the state if fixed up.
        */
        class Context {
            MONGO_DISALLOW_COPYING(Context);
        public:
            /** this is probably what you want */
            Context(OperationContext* txn, const std::string& ns, bool doVersion = true);

            /**
             * Below still calls _finishInit, but assumes database has already been acquired
             * or just created.
             */
            Context(OperationContext* txn,
                    const std::string& ns,
                    Database* db,
                    bool justCreated);

            /**
             * note: this does not call _finishInit -- i.e., does not call
             * ensureShardVersionOKOrThrow for example.
             * see also: reset().
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
                return _c.db()->getCollection(_nss.ns());
            }

            Context& ctx() { return _c; }

        private:
            OperationContext* _txn;
            NamespaceString _nss;
            AutoGetOrCreateDb _autodb;
            Lock::CollectionLock _collk;
            Context _c;
            Collection* _collection;
        };
    }; // class Client

    /** get the Client object for this thread. */
    inline Client& cc() {
        Client * c = currentClient.get();
        verify( c );
        return *c;
    }

    inline bool haveClient() { return currentClient.get() != NULL; }

};
