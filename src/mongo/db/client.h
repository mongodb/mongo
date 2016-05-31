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

#include "mongo/db/client_basic.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/service_context.h"
#include "mongo/platform/random.h"
#include "mongo/platform/unordered_set.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/concurrency/spin_lock.h"
#include "mongo/util/concurrency/threadlocal.h"

namespace mongo {

class AbstractMessagingPort;
class Collection;
class OperationContext;

namespace transport {
class Session;
}  // namespace transport

typedef long long ConnectionId;

/** the database's concept of an outside "client" */
class Client : public ClientBasic {
public:
    /**
     * Creates a Client object and stores it in TLS for the current thread.
     *
     * An unowned pointer to a transport::Session may optionally be provided. If 'session'
     * is non-null, then it will be used to augment the thread name, and for reporting purposes.
     *
     * If provided, 'session' must outlive the newly-created Client object. Client::destroy() may
     * be used to help enforce that the Client does not outlive 'session.'
     */
    static void initThread(const char* desc, transport::Session* session = nullptr);
    static void initThread(const char* desc,
                           ServiceContext* serviceContext,
                           transport::Session* session);

    /**
     * Inits a thread if that thread has not already been init'd, setting the thread name to
     * "desc".
     */
    static void initThreadIfNotAlready(const char* desc);

    /**
     * Inits a thread if that thread has not already been init'd, using the existing thread name
     */
    static void initThreadIfNotAlready();

    /**
     * Destroys the Client object stored in TLS for the current thread. The current thread must have
     * a Client.
     *
     * If destroy() is not called explicitly, then the Client stored in TLS will be destroyed upon
     * exit of the current thread.
     */
    static void destroy();

    std::string clientAddress(bool includePort = false) const;
    const std::string& desc() const {
        return _desc;
    }

    void reportState(BSONObjBuilder& builder);

    // Ensures stability of the client's OperationContext. When the client is locked,
    // the OperationContext will not disappear.
    void lock() {
        _lock.lock();
    }
    void unlock() {
        _lock.unlock();
    }

    /**
     * Makes a new operation context representing an operation on this client.  At most
     * one operation context may be in scope on a client at a time.
     */
    ServiceContext::UniqueOperationContext makeOperationContext();

    /**
     * Sets the active operation context on this client to "txn", which must be non-NULL.
     *
     * It is an error to call this method if there is already an operation context on Client.
     * It is an error to call this on an unlocked client.
     */
    void setOperationContext(OperationContext* txn);

    /**
     * Clears the active operation context on this client.
     *
     * There must already be such a context set on this client.
     * It is an error to call this on an unlocked client.
     */
    void resetOperationContext();

    /**
     * Gets the operation context active on this client, or nullptr if there is no such context.
     *
     * It is an error to call this method on an unlocked client, or to use the value returned
     * by this method while the client is not locked.
     */
    OperationContext* getOperationContext() {
        return _txn;
    }

    // TODO(spencer): SERVER-10228 SERVER-14779 Remove this/move it fully into OperationContext.
    bool isInDirectClient() const {
        return _inDirectClient;
    }
    void setInDirectClient(bool newVal) {
        _inDirectClient = newVal;
    }

    ConnectionId getConnectionId() const {
        return _connectionId;
    }
    bool isFromUserConnection() const {
        return _connectionId > 0;
    }

    PseudoRandom& getPrng() {
        return _prng;
    }

private:
    friend class ServiceContext;
    Client(std::string desc, ServiceContext* serviceContext, transport::Session* session = nullptr);


    // Description for the client (e.g. conn8)
    const std::string _desc;

    // OS id of the thread, which owns this client
    const stdx::thread::id _threadId;

    // > 0 for things "conn", 0 otherwise
    const ConnectionId _connectionId;

    // Protects the contents of the Client (such as changing the OperationContext, etc)
    SpinLock _lock;

    // Whether this client is running as DBDirectClient
    bool _inDirectClient = false;

    // If != NULL, then contains the currently active OperationContext
    OperationContext* _txn = nullptr;

    PseudoRandom _prng;
};

/** get the Client object for this thread. */
Client& cc();

bool haveClient();
};
