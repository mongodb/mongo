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

#include <boost/scoped_ptr.hpp>
#include <boost/thread/thread.hpp>

#include "mongo/db/client_basic.h"
#include "mongo/db/lasterror.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/platform/unordered_set.h"
#include "mongo/util/concurrency/spin_lock.h"
#include "mongo/util/concurrency/threadlocal.h"

namespace mongo {

    class Collection;
    class AbstractMessagingPort;

    TSP_DECLARE(Client, currentClient)

    typedef long long ConnectionId;

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
        static void initThread(const char* desc,
                               ServiceContext* serviceContext,
                               AbstractMessagingPort* mp);

        /**
         * Inits a thread if that thread has not already been init'd, setting the thread name to
         * "desc".
         */
        static void initThreadIfNotAlready(const char* desc) {
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
        void shutdown();

        std::string clientAddress(bool includePort = false) const;
        const std::string& desc() const { return _desc; }

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
        bool isInDirectClient() const { return _inDirectClient; }
        void setInDirectClient(bool newVal) { _inDirectClient = newVal; }

        ConnectionId getConnectionId() const { return _connectionId; }
        bool isFromUserConnection() const { return _connectionId > 0; }

    private:
        Client(const std::string& desc,
               ServiceContext* serviceContext,
               AbstractMessagingPort *p = 0);


        // Description for the client (e.g. conn8)
        const std::string _desc;

        // OS id of the thread, which owns this client
        const boost::thread::id _threadId;

        // > 0 for things "conn", 0 otherwise
        const ConnectionId _connectionId;

        // Protects the contents of the Client (such as changing the OperationContext, etc)
        mutable SpinLock _lock;

        // Whether this client is running as DBDirectClient
        bool _inDirectClient;

        // If != NULL, then contains the currently active OperationContext
        OperationContext* _txn;
    };

    /** get the Client object for this thread. */
    inline Client& cc() {
        Client * c = currentClient.get();
        verify( c );
        return *c;
    }

    inline bool haveClient() { return currentClient.get() != NULL; }

};
