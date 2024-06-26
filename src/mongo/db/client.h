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

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <memory>
#include <string>
#include <utility>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/random.h"
#include "mongo/stdx/thread.h"
#include "mongo/transport/session.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/spin_lock.h"
#include "mongo/util/concurrency/thread_name.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/decorable.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/net/ssl_peer_info.h"
#include "mongo/util/uuid.h"

namespace mongo {

class OperationContext;
class ThreadClient;

typedef long long ConnectionId;

/**
 * The database's concept of an outside "client".
 */
class Client final : public Decorable<Client> {
public:
    /**
     * These tags classify the connections associated with Clients and reasons to keep them open.
     * When closing connections, these tags can be used to filter out the type of connections that
     * should be kept open.
     *
     * Clients that are not yet classified yet are marked as kPending. The classification occurs
     * during the processing of "hello" commands.
     */
    using TagMask = uint32_t;

    // Client's connections should be closed.
    static constexpr TagMask kEmptyTagMask = 0;

    // Client's connection should be kept open on replication member rollback or removal.
    static constexpr TagMask kKeepOpen = 1;

    // Client is internal and their max wire version is not less than the max wire version of
    // this server. Connection should be kept open.
    static constexpr TagMask kLatestVersionInternalClientKeepOpen = 2;

    // Client is external and connection should be kept open.
    static constexpr TagMask kExternalClientKeepOpen = 4;

    // Client has not been classified yet and should be kept open.
    static constexpr TagMask kPending = 1 << 31;

    /** Used as a placeholder argument to avoid specifying a session. */
    static std::shared_ptr<transport::Session> noSession() {
        return {};
    }

    /**
     * Creates a Client object and stores it in TLS for the current thread.
     *
     * If `session` is non-null, then it will be used to augment the thread name
     * and for reporting purposes. Its ref count will be bumped by this Client.
     */
    static void initThread(StringData desc,
                           Service* service,
                           std::shared_ptr<transport::Session> session = noSession());

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

    ~Client() override;

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
        MONGO_verify(_session);
        return _session->remote();
    }

    /**
     * Overwrites the Service for this client. To be used by the replica set endpoint only.
     */
    Service* setService(Service* service) {
        return _service = service;
    }

    /**
     * Returns the Service that owns this client.
     */
    Service* getService() const {
        return _service;
    }

    /**
     * Returns true if this client is connected to the router port of a mongod with router role.
     */
    bool isRouterClient() const {
        return _isRouterClient;
    }

    /**
     * Returns the ServiceContext that owns this client session context.
     */
    ServiceContext* getServiceContext() const {
        return getService()->getServiceContext();
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

    // Ensures stability of everything under the client object, most notably the associated
    // OperationContext.
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
     * Used to mark system operations that are not allowed to be killed by the stepdown
     * thread. This should only be called once per Client and only from system connections. The
     * Client should be locked by the caller. We should minimize the usage of this function because
     * improper usage could lead to a deadlock. More context comes as follows.
     *
     * How does this kill operation process work?
     * To be more clear, the kill operation is actually an interruption. In order to get the RSTL
     * lock during stepUp/stepDown, the replication coordinator will start a RstlKillOpThread to
     * interrupt all threads that may block it for the RSTL lock. The RstlKillOpThread will loop
     * through all threads and find out the threads that have ever taken a global lock in S/X/IX
     * mode. It will interrupt these threads by interrupting their opCtx, which will cause an
     * InterruptedDueToReplStateChange error to be thrown when the thread checks interruption on the
     * opCtx. In addition to those threads hold global locks, a thread could also be interrupted if
     * it is explicitly marked as alwaysInterruptAtStepDownOrUp or waiting on prepare conflicts.
     *
     * What should I consider if I introduced a new thread?
     * - Whether the new thread ever takes any global lock in S/IX/X mode? If not, the stepdown
     *   thread won't interrupt the new thread so we should leave it as killable. Even if the thread
     *   takes global lock, the best practice should be making the new thread killable and handle
     *   the interruption properly.
     * - It's always better to write the code in a way that can catch the
     *   InterruptedDueToReplStateChange error and recover from there. This helps make the thread
     *   killable in a safer way and can prevent deadlock from unkillable thread.
     * - If the thread has to be unkillable, it would be helpful to leave the reason in a comment so
     *   if it becomes an issue, it will be easier to find the cause.
     */
    void setSystemOperationUnkillableByStepdown(WithLock) {
        // This can only be changed once for system operations.
        invariant(isFromSystemConnection());
        invariant(_systemOperationKillable);
        _systemOperationKillable = false;
    }

    /**
     * Used to determine whether a system operation is allowed to be killed by the stepdown thread.
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

    bool isInternalClient() const {
        return _isInternalClient;
    }

    void setIsInternalClient(bool isInternalClient) {
        _isInternalClient = isInternalClient;
    }

    /**
     * Sets the error code that operations associated with this client will be killed with if the
     * underlying ingress session disconnects.
     */
    void setDisconnectErrorCode(ErrorCodes::Error code) {
        _disconnectErrorCode = code;
    }

    ErrorCodes::Error getDisconnectErrorCode() {
        return _disconnectErrorCode;
    }

    /**
     * Atomically set all of the connection tags specified in the 'tagsToSet' bit field. If the
     * 'kPending' tag is set, indicating that no tags have yet been specified for the connection,
     * this function also clears that tag as part of the same atomic operation.
     *
     * The 'kPending' tag is only for new connections; callers should not set it directly.
     */
    void setTags(TagMask tagsToSet);

    /**
     * Atomically clears all of the connection tags specified in the 'tagsToUnset' bit field. If
     * the 'kPending' tag is set, indicating that no tags have yet been specified for the session,
     * this function also clears that tag as part of the same atomic operation.
     */
    void unsetTags(TagMask tagsToUnset);

    /**
     * Loads the connection tags, passes them to 'mutateFunc' and then stores the result of that
     * call as the new connection tags, all in one atomic operation.
     *
     * In order to ensure atomicity, 'mutateFunc' may get called multiple times, so it should not
     * perform expensive computations or operations with side effects.
     *
     * If the 'kPending' tag is set originally, mutateTags() will unset it regardless of the result
     * of the 'mutateFunc' call. The 'kPending' tag is only for new connections; callers should
     * never try to set it.
     */
    void mutateTags(const std::function<TagMask(TagMask)>& mutateFunc);

    TagMask getTags() const;

    /**
     * Returns the associated port for this client.
     */
    int getLocalPort() const;

private:
    friend class ServiceContext;
    friend class ThreadClient;

    Client(std::string desc, Service* service, std::shared_ptr<transport::Session> session);

    /**
     * Sets the active operation context on this client to "opCtx".
     */
    void _setOperationContext(OperationContext* opCtx);

    Service* _service;

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

    // Whether this client is connected to the router port of a mongod with router role.
    const bool _isRouterClient = false;

    UUID _uuid;

    // Indicates that this client is internal to the cluster.
    bool _isInternalClient{false};

    ErrorCodes::Error _disconnectErrorCode = ErrorCodes::ClientDisconnect;

    AtomicWord<TagMask> _tags;
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
    /**
     * The overload set for this constructor bears some explanation.
     * The primary fully-populated parameter list is:
     *     StringData desc
     *     Service* service
     *     std::shared_ptr<transport::Session> session
     *
     * However, a full set of 2 variations on this constructor are accepted.
     *
     * A) The `desc` can be omitted, and will default to `getThreadName()`.
     *
     * B) The `session` can be omitted, and will default to `Client::noSession()`.
     */
    ThreadClient(StringData desc, Service* service, std::shared_ptr<transport::Session> session);

    /** A) Repeat, with `desc` omitted. */
    ThreadClient(Service* service, std::shared_ptr<transport::Session> session)
        : ThreadClient(getThreadName(), service, std::move(session)) {}

    /** B) Repeat all previous constructors, with `session` omitted. */
    ThreadClient(StringData desc, Service* service)
        : ThreadClient{desc, service, Client::noSession()} {}
    explicit ThreadClient(Service* service) : ThreadClient{service, Client::noSession()} {}

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
    explicit AlternativeClientRegion(ServiceContext::UniqueClient& clientToUse);

    ~AlternativeClientRegion();
    AlternativeClientRegion(const AlternativeClientRegion&) = delete;
    AlternativeClientRegion(AlternativeClientRegion&&) = delete;
    void operator=(const AlternativeClientRegion&) = delete;

    Client* get() const;
    Client* operator->() const {
        return get();
    }
    Client& operator*() const {
        return *get();
    }

private:
    ServiceContext::UniqueClient _originalClient;
    ServiceContext::UniqueClient* const _alternateClient;
};

/** get the Client object for this thread. */
Client& cc();

bool haveClient();

}  // namespace mongo
