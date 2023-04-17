/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

/* @file db/client.h

   "Client" represents a connection to the database (the server-side) and corresponds
   to an open socket (or logical connection if pooling on sockets) from a client.

   todo: switch to asio...this will fit nicely with that.
*/

#pragma once

#include <boost/optional.hpp>

#include "mongo/db/namespace_string.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/random.h"
#include "mongo/stdx/thread.h"
#include "mongo/transport/session.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/spin_lock.h"
#include "mongo/util/decorable.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/net/ssl_peer_info.h"
#include "mongo/util/uuid.h"

namespace mongo {

class Locker;
class OperationContext;
class ThreadClient;

typedef long long ConnectionId;

/**
 * The database's concept of an outside "client".
 * */
class Client final : public Decorable<Client> {
public:
    /**
     * Creates a Client object and stores it in TLS for the current thread.
     *
     * An unowned pointer to a transport::Session may optionally be provided. If 'session'
     * is non-null, then it will be used to augment the thread name, and for reporting purposes.
     *
     * If provided, session's ref count will be bumped by this Client.
     */
    static void initThread(StringData desc, std::shared_ptr<transport::Session> session = nullptr);
    static void initThread(StringData desc,
                           ServiceContext* serviceContext,
                           std::shared_ptr<transport::Session> session);

    /**
     * Moves client into the thread_local for this thread. After this call, Client::getCurrent
     * and cc() will return client.get(). The client will be destroyed when the thread exits
     * or the ThreadClient RAII helper exits its scope.
     */
    static void setCurrent(ServiceContext::UniqueClient client);

    /**
     * Releases the client being managed by the thread_local for this thread. After this call
     * cc() will crash the server and Client::getCurrent() will return nullptr until either
     * Client::initThread() or Client::setCurrent() is called.
     *
     * The client will be released to the caller.
     */
    static ServiceContext::UniqueClient releaseCurrent();

    static Client* getCurrent();

    bool getIsLocalHostConnection() {
        if (!hasRemote()) {
            return false;
        }
        return getRemote().isLocalHost();
    }

    bool hasRemote() const {
        return (_session != nullptr);
    }

    HostAndPort getRemote() const {
        verify(_session);
        return _session->remote();
    }

    /**
     * Returns the ServiceContext that owns this client session context.
     */
    ServiceContext* getServiceContext() const {
        return _serviceContext;
    }

    /**
     * Returns the Session to which this client is bound, if any.
     */
    const std::shared_ptr<transport::Session>& session() const& {
        return _session;
    }

    boost::optional<std::string> getSniNameForSession() const {
        return _session ? SSLPeerInfo::forSession(_session).sniName() : boost::none;
    }

    std::shared_ptr<transport::Session> session() && {
        return std::move(_session);
    }

    std::string clientAddress(bool includePort = false) const;
    const std::string& desc() const {
        return _desc;
    }

    void reportState(BSONObjBuilder& builder);

    // Ensures stability of the client's OperationContext. When the client is locked,
    // the OperationContext and the Locker within it will not disappear.
    void lock() {
        _lock.lock();
    }
    void unlock() {
        _lock.unlock();
    }
    bool try_lock() {
        return _lock.try_lock();
    }

    /**
     * Makes a new operation context representing an operation on this client.  At most
     * one operation context may be in scope on a client at a time.
     *
     * If provided, the LogicalSessionId links this operation to a logical session.
     */
    ServiceContext::UniqueOperationContext makeOperationContext();

    /**
     * Gets the operation context active on this client, or nullptr if there is no such context.
     *
     * It is an error to call this method on an unlocked client, or to use the value returned
     * by this method while the client is not locked.
     */
    OperationContext* getOperationContext() {
        return _opCtx;
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
    bool isFromSystemConnection() const {
        return _connectionId == 0;
    }

    const auto& getUUID() const {
        return _uuid;
    }

    /**
     * Used to mark system operations that are not allowed to be killed by the stepdown process.
     * This should only be called once per Client and only from system connections. The Client
     * should be locked by the caller.
     */
    void setSystemOperationUnkillableByStepdown(WithLock) {
        // This can only be changed once for system operations.
        invariant(isFromSystemConnection());
        invariant(_systemOperationKillable);
        _systemOperationKillable = false;
    }

    /**
     * Used to determine whether a system operation is allowed to be killed by the stepdown process.
     * The Client should be locked by the caller.
     */
    bool canKillSystemOperationInStepdown(WithLock) const {
        // Should only be called on system operations.
        invariant(isFromSystemConnection());
        return _systemOperationKillable;
    }

    PseudoRandom& getPrng() {
        return _prng;
    }

    /**
     * Safely swaps the locker in the OperationContext, releasing the old locker to the caller.
     * Locks this Client to do this safely.
     */
    std::unique_ptr<Locker> swapLockState(std::unique_ptr<Locker> locker);

    /**
     * Checks if there is an active currentOp associated with this client.
     * The definition of active varies between User and System connections.
     * Note that the caller must hold the client lock.
     */
    bool hasAnyActiveCurrentOp() const;

    /**
     * Signal the client's OperationContext that it has been killed.
     * Any future OperationContext on this client will also receive a kill signal.
     */
    void setKilled() noexcept;

    /**
     * Get the state for killing the client's OperationContext.
     */
    bool getKilled() const noexcept {
        return _killed.loadRelaxed();
    }

    /**
     * Whether this client supports the hello command, which indicates that the server
     * can return "not primary" error messages.
     */
    bool supportsHello() const {
        return _supportsHello;
    }

    /**
     * Will be set to true if the client sent { helloOk: true } when opening a
     * connection to the server. Defaults to false.
     */
    void setSupportsHello(bool newVal) {
        _supportsHello = newVal;
    }

private:
    friend class ServiceContext;
    friend class ThreadClient;
    explicit Client(std::string desc,
                    ServiceContext* serviceContext,
                    std::shared_ptr<transport::Session> session);

    /**
     * Sets the active operation context on this client to "opCtx".
     */
    void _setOperationContext(OperationContext* opCtx) {
        _opCtx = opCtx;
    }

    ServiceContext* const _serviceContext;
    const std::shared_ptr<transport::Session> _session;

    // Description for the client (e.g. conn8)
    const std::string _desc;

    // > 0 for things "conn", 0 otherwise
    const ConnectionId _connectionId;

    // Protects the contents of the Client (such as changing the OperationContext, etc)
    SpinLock _lock;

    // Whether this client is running as DBDirectClient
    bool _inDirectClient = false;

    // If != NULL, then contains the currently active OperationContext
    OperationContext* _opCtx = nullptr;

    // If the active system client operation is allowed to be killed.
    bool _systemOperationKillable = true;

    PseudoRandom _prng;

    AtomicWord<bool> _killed{false};

    // Whether this client used { helloOk: true } when opening its connection, indicating that
    // it supports the hello command.
    bool _supportsHello = false;

    UUID _uuid;
};

/**
 * RAII-style Client helper to manage its lifecycle.
 * Instantiates a client on the current thread, which remains bound to this thread so long as the
 * instance of ThreadClient is in scope.
 *
 * Swapping the managed Client by ThreadClient with AlternativeClientRegion is permitted so long as
 * the AlternativeClientRegion is not used beyond the scope of ThreadClient.
 *
 * Calling Client::releaseCurrent() is not permitted on a Client managed by the ThreadClient and
 * will invariant once ThreadClient goes out of scope.
 */
class ThreadClient {
public:
    explicit ThreadClient(ServiceContext* serviceContext);
    explicit ThreadClient(StringData desc,
                          ServiceContext* serviceContext,
                          std::shared_ptr<transport::Session> session = nullptr);
    ~ThreadClient();
    ThreadClient(const ThreadClient&) = delete;
    ThreadClient(ThreadClient&&) = delete;
    void operator=(const ThreadClient&) = delete;

    Client* get() const;
    Client* operator->() const {
        return get();
    }
    Client& operator*() const {
        return *get();
    }

private:
    ThreadNameRef _originalThreadName;
};

/**
 * Utility class to temporarily swap which client is bound to the running thread.
 *
 * Use this class to bind a client to the current thread for the duration of the
 * AlternativeClientRegion's lifetime, restoring the prior client, if any, at the
 * end of the block.
 */
class AlternativeClientRegion {
public:
    explicit AlternativeClientRegion(ServiceContext::UniqueClient& clientToUse)
        : _alternateClient(&clientToUse) {
        invariant(clientToUse);
        if (Client::getCurrent()) {
            _originalClient = Client::releaseCurrent();
        }
        Client::setCurrent(std::move(*_alternateClient));
    }

    ~AlternativeClientRegion() {
        *_alternateClient = Client::releaseCurrent();
        if (_originalClient) {
            Client::setCurrent(std::move(_originalClient));
        }
    }

private:
    ServiceContext::UniqueClient _originalClient;
    ServiceContext::UniqueClient* const _alternateClient;
};


/** get the Client object for this thread. */
Client& cc();

bool haveClient();
}  // namespace mongo
