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

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/db/baton.h"
#include "mongo/db/operation_id.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/rwmutex.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/transport/session.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/concurrency/lock_free_read_list.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/decorable.h"
#include "mongo/util/hierarchical_acquisition.h"
#include "mongo/util/modules_incompletely_marked_header.h"
#include "mongo/util/observable_mutex.h"
#include "mongo/util/periodic_runner.h"
#include "mongo/util/synchronized_value.h"
#include "mongo/util/tick_source.h"
#include "mongo/util/uuid.h"

#include <cstdint>
#include <functional>
#include <iostream>
#include <list>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

class AbstractMessagingPort;
class Client;
class OperationContext;
class OpObserver;
class Service;
class ServiceContext;
class ServiceEntryPoint;

namespace transport {
class TransportLayerManager;
}  // namespace transport

/**
 * Users may provide an OperationKey when sending a command request as a stable token by which to
 * refer to an operation (and thus an OperationContext). An OperationContext is not required to have
 * an OperationKey. The presence of an OperationKey implies that the client is either closely
 * tracking or speculative executing its command.
 */
using OperationKey = UUID;

namespace service_context_detail {
/**
 * A synchronized owning pointer to avoid setters racing with getters.
 * This only guarantees that getters receive a coherent value, and
 * not that the pointer is still valid.
 *
 * The kernel of operations is `set` and and `get`, others ops are sugar.
 */
template <typename T>
class SyncUnique {
public:
    SyncUnique() = default;
    explicit SyncUnique(std::unique_ptr<T> p) {
        set(std::move(p));
    }

    ~SyncUnique() {
        set(nullptr);
    }

    SyncUnique& operator=(std::unique_ptr<T> p) {
        set(std::move(p));
        return *this;
    }

    void set(std::unique_ptr<T> p) {
        delete _ptr.swap(p.release());
    }

    T* get() const {
        return _ptr.load();
    }

    T* operator->() const {
        return get();
    }

    T& operator*() const {
        return *get();
    }

    explicit operator bool() const {
        return static_cast<bool>(get());
    }

    std::unique_ptr<T> swap(std::unique_ptr<T> p) noexcept {
        return std::unique_ptr<T>{_ptr.swap(p.release())};
    }

private:
    AtomicWord<T*> _ptr{nullptr};
};

template <typename T>
auto makeLockHandleForObjectLock(T* object) {
    return stdx::unique_lock(*object);
}

/**
 * Wraps a lockable object of type `T`, locking it at construction and releasing the lock when
 * destroyed.
 */
template <typename T, typename MutexType = T>
class ObjectLock {
public:
    ObjectLock() = default;
    explicit ObjectLock(T* obj) : _lk(makeLockHandleForObjectLock(obj)), _object(obj) {}

    T& operator*() const noexcept {
        invariant(_object);
        return *_object;
    }

    T* operator->() const noexcept {
        return _object;
    }

    explicit operator bool() const noexcept {
        return !!_object;
    }

    explicit(false) operator WithLock() const noexcept {
        return WithLock(_lk);
    }

private:
    stdx::unique_lock<MutexType> _lk;
    T* _object{};
};
}  // namespace service_context_detail

class ClientLock : public service_context_detail::ObjectLock<Client> {
public:
    ClientLock() = default;
    explicit ClientLock(Client* client);
};

/**
 * This is for internal use by `ServiceContext`. Avoid using it to lock `ServiceContext` as it will
 * block normal server operations.
 */
using ServiceContextLock =
    service_context_detail::ObjectLock<ServiceContext, ObservableMutex<stdx::mutex>>;

/**
 * Classes that implement this interface can receive notification on killOp.
 *
 * See registerKillOpListener() for more information,
 * including limitations on the lifetime of registered listeners.
 */
class MONGO_MOD_OPEN KillOpListenerInterface {
public:
    KillOpListenerInterface(const KillOpListenerInterface&) = delete;
    KillOpListenerInterface& operator=(const KillOpListenerInterface&) = delete;

    /**
     * Will be called *after* the operation (i.e. `opCtx`) has been killed, while holding its client
     * lock. Must not fail.
     */
    virtual void interrupt(ClientLock&, OperationContext*) = 0;

    /**
     * Will be called *after* all operations have been killed and provided with an error code, while
     * holding the lock for `ServiceContext`. Must not fail.
     */
    virtual void interruptAll(ServiceContextLock&) = 0;

protected:
    KillOpListenerInterface() = default;

    // Should not delete through a pointer of this type
    virtual ~KillOpListenerInterface() = default;
};

/**
 * Registers a function to execute on new ServiceContexts or Services when they are
 * created and optionally also register a function to execute before those contexts are
 * destroyed. Choose Service or ServiceContext depending on the template argument.
 *
 * Construct instances of this type during static initialization only, as they register
 * MONGO_INITIALIZERS.
 */
template <typename T>
class ConstructorActionRegistererType {

public:
    using ConstructorAction = std::function<void(T*)>;
    using DestructorAction = std::function<void(T*)>;

    /**
     * Register functions of type ConstructorAction and DestructorAction using an
     * instance of ConstructorActionRegisterer, called on construction of objects
     * Type T.
     */
    class ConstructorDestructorActions {
    public:
        ConstructorDestructorActions(ConstructorAction constructor, DestructorAction destructor)
            : _constructor(std::move(constructor)), _destructor(std::move(destructor)) {}

        void onCreate(T* service) const {
            _constructor(service);
        }
        void onDestroy(T* service) const {
            _destructor(service);
        }

    private:
        ConstructorAction _constructor;
        DestructorAction _destructor;
    };

    /**
     * Accessor function to get the global list of ServiceContext constructor and destructor
     * functions.
     */
    static std::list<ConstructorDestructorActions>& registeredConstructorActions() {
        static std::list<ConstructorDestructorActions> cal;
        return cal;
    }

    /**
     * This constructor registers a constructor and optional destructor with the given
     * "name" and no prerequisite constructors or mongo initializers.
     */
    ConstructorActionRegistererType(std::string name,
                                    ConstructorAction constructor,
                                    DestructorAction destructor = {});

    /**
     * This constructor registers a constructor and optional destructor with the given
     * "name", and a list of names of prerequisites, "prereqs".
     *
     * The named constructor will run after all of its prereqs successfully complete,
     * and the corresponding destructor, if provided, will run before any of its
     * prerequisites execute.
     */
    ConstructorActionRegistererType(std::string name,
                                    std::vector<std::string> prereqs,
                                    ConstructorAction constructor,
                                    DestructorAction destructor = {});

    /**
     * This constructor registers a constructor and optional destructor with the given
     * "name", a list of names of prerequisites, "prereqs", and a list of names of dependents,
     * "dependents".
     *
     * The named constructor will run after all of its prereqs successfully complete,
     * and the corresponding destructor, if provided, will run before any of its
     * prerequisites execute. The dependents will run after this constructor and
     * the corresponding destructor, if provided, will run after any of its
     * dependents execute.
     */
    ConstructorActionRegistererType(std::string name,
                                    std::vector<std::string> prereqs,
                                    std::vector<std::string> dependents,
                                    ConstructorAction constructor,
                                    DestructorAction destructor = {});

private:
    using ConstructorActionListIterator =
        typename std::list<ConstructorDestructorActions>::iterator;
    ConstructorActionListIterator _iter;
    boost::optional<GlobalInitializerRegisterer> _registerer;
};


/**
 * Used to mark operations that are not allowed to be killed by the stepdown thread.
 *
 * Do not add any new uses of unkillable operations without very careful deliberation. Improper
 * usage could lead to a deadlock, and plans are in place via SERVER-74658 to make as many
 * operations as possible killable.
 *
 * How does the operation killing process work?
 *   To be clear, an operation kill is actually an interruption. In order to get the RSTL lock
 *   lock during stepUp/stepDown, the replication coordinator will start a RstlKillOpThread to
 *   interrupt all threads that may block it for the RSTL lock. The RstlKillOpThread will loop
 *   through all threads and find out the threads that have ever taken a global lock in S/X/IX
 *   mode. It will interrupt these threads by interrupting their opCtx. Interrupting their opCtx
 *   will cause an InterruptedDueToReplStateChange error to be thrown when the thread checks for
 *   interruption on that opCtx.
 *
 *   In addition to threads holding global locks, a thread could also be interrupted if it:
 *   - is explicitly marked as alwaysInterruptAtStepDownOrUp, or if it
 *   - is waiting on prepare conflicts.
 *
 * What should I consider if I want to introduce a new thread?
 *   Does the new thread ever take any global lock in S/IX/X mode? If not, the stepdown thread
 *   won't interrupt the new thread, so we should leave it as killable. Even if the thread takes
 *   takes the global lock, the best practice should still be making the new thread killable and
 *   handling the interruption properly. It's always better to write the code in a way that can
 *   catch the InterruptedDueToReplStateChange error and recover from there, to avoid locking
 *   issues.
 *
 *   If the thread has to be unkillable, a comment must be left explaining the reason. This will
 *   help future diagnosability.
 *
 * TODO(SERVER-74658): Remove this type if all theads are found to be killable.
 */
enum class ClientOperationKillableByStepdown : bool {};

/**
 * Class representing the context of a service, such as a MongoD database service or
 * a MongoS routing service.
 *
 * A ServiceContext is the root of a hierarchy of contexts.  A ServiceContext owns
 * zero or more Clients, which in turn each own OperationContexts.
 */
class ServiceContext final : public Decorable<ServiceContext> {
    ServiceContext(const ServiceContext&) = delete;
    ServiceContext& operator=(const ServiceContext&) = delete;
    template <typename T>
    using SyncUnique = service_context_detail::SyncUnique<T>;

public:
    /**
     * Observer interface implemented to hook client and operation context creation and
     * destruction.
     */
    class ClientObserver {
    public:
        virtual ~ClientObserver() = default;

        /**
         * Hook called after a new client "client" is created on a service
         * managed by this ServiceContext.
         *
         * For a given client and registered instance of ClientObserver, if onCreateClient
         * returns without throwing an exception, onDestroyClient will be called when "client"
         * is deleted.
         */
        virtual void onCreateClient(Client* client) = 0;

        /**
         * Hook called on a "client" created by a service before deleting "client".
         *
         * Like a destructor, must not throw exceptions.
         */
        virtual void onDestroyClient(Client* client) = 0;

        /**
         * Hook called after a new operation context is created on a client by
         * service->makeOperationContext(client)  or client->makeOperationContext().
         *
         * For a given operation context and registered instance of ClientObserver, if
         * onCreateOperationContext returns without throwing an exception,
         * onDestroyOperationContext will be called when "opCtx" is deleted.
         */
        virtual void onCreateOperationContext(OperationContext* opCtx) = 0;

        /**
         * Hook called on a "opCtx" created by a service before deleting "opCtx".
         * Note that this hook is called before any other work is done in the
         * OperationContext destructor, meaning the OperationContext is still
         * valid and registered with the ServiceContext when this hook is
         * executed.
         *
         * Like a destructor, must not throw exceptions.
         */
        virtual void onDestroyOperationContext(OperationContext* opCtx) = 0;
    };

    using ClientList = LockFreeReadList<Client*>;
    using ClientMap = stdx::unordered_map<Client*, typename ClientList::Entry*>;

    /**
     * Cursor for enumerating the live Client objects belonging to a ServiceContext.
     * This is just a wrapper, so prefer using `makeClientsCursor` instead.
     */
    class LockedClientsCursor {
    public:
        /**
         * Constructs a cursor for enumerating the clients of "service", blocking "service" from
         * destroying the Client object actively referenced by the cursor.
         */
        explicit LockedClientsCursor(ServiceContext* svcCtx)
            : _cursor(svcCtx->makeClientsCursor()) {}

        /**
         * Returns the next client in the enumeration, or nullptr if there are no more clients.
         */
        Client* next() {
            if (std::exchange(_mustGetNext, true)) {
                _cursor.next();
            }
            return _cursor ? _cursor.value() : nullptr;
        }

    private:
        ClientList::Cursor _cursor;
        bool _mustGetNext = false;
    };

    /**
     * Special deleter used for cleaning up ServiceContext objects.
     * See UniqueServiceContext, below.
     */
    class ServiceContextDeleter {
    public:
        void operator()(ServiceContext* sc) const;
    };

    using UniqueServiceContext = std::unique_ptr<ServiceContext, ServiceContextDeleter>;
    using ConstructorActionRegisterer = ConstructorActionRegistererType<ServiceContext>;

    /**
     * Special deleter used for cleaning up Client objects owned by a ServiceContext.
     * See UniqueClient, below.
     */
    class ClientDeleter {
    public:
        void operator()(Client* client) const;
    };

    /**
     * This is the unique handle type for Clients created by a ServiceContext.
     */
    using UniqueClient = std::unique_ptr<Client, ClientDeleter>;

    /**
     * Special deleter used for cleaning up OperationContext objects owned by a ServiceContext.
     * See UniqueOperationContext, below.
     */
    class OperationContextDeleter {
    public:
        void operator()(OperationContext* opCtx) const;
    };

    /**
     * This is the unique handle type for OperationContexts created by a ServiceContext.
     */
    using UniqueOperationContext = std::unique_ptr<OperationContext, OperationContextDeleter>;

    /**
     * Register a function of this type using  an instance of ConstructorActionRegisterer,
     * below, to cause the function to be executed on new ServiceContext instances.
     */
    using ConstructorAction = std::function<void(ServiceContext*)>;

    /**
     * Register a function of this type using an instance of ConstructorActionRegisterer,
     * below, to cause the function to be executed on ServiceContext instances before they
     * are destroyed.
     */
    using DestructorAction = std::function<void(ServiceContext*)>;

    /**
     * Factory function for making instances of ServiceContext. It is the only means by which they
     * should be created.
     */
    static UniqueServiceContext make(std::unique_ptr<ClockSource> fastClockSource = nullptr,
                                     std::unique_ptr<ClockSource> preciseClockSource = nullptr,
                                     std::unique_ptr<TickSource> tickSource = nullptr);

    ServiceContext(std::unique_ptr<ClockSource> fastClockSource,
                   std::unique_ptr<ClockSource> preciseClockSource,
                   std::unique_ptr<TickSource> tickSource);
    ~ServiceContext() override;

    /**
     * Registers an observer of lifecycle events on Clients created by this ServiceContext.
     *
     * See the ClientObserver type, above, for details.
     *
     * All calls to registerClientObserver must complete before ServiceContext
     * is used in multi-threaded operation, or is used to create clients via calls
     * to makeClient on Service instances managed by this ServiceContext.
     */
    void registerClientObserver(std::unique_ptr<ClientObserver> observer);

    /** Internal: Called by Service->makeClient. */
    UniqueClient makeClientForService(std::string desc,
                                      std::shared_ptr<transport::Session> session,
                                      ClientOperationKillableByStepdown killable,
                                      Service* service);

    /**
     * Creates a new OperationContext on "client".
     *
     * "client" must not have an active operation context.
     *
     */
    UniqueOperationContext makeOperationContext(Client* client);

    //
    // Storage
    //

    /**
     * Sets the storage engine for this instance. May be called up to once per instance, unless
     * clearStorageEngine() is called in which it may be called once after each call to
     * clearStorageEngine().
     */
    void setStorageEngine(std::unique_ptr<StorageEngine> engine);

    /**
     * Takes a function and applies it to all service objects associated with the service context.
     * The function must accept a service as an argument.
     */
    template <typename F>
    void applyToAllServices(F fn) {
        if (auto service = getService(ClusterRole::RouterServer); service) {
            fn(service);
        }

        if (auto service = getService(ClusterRole::ShardServer); service) {
            fn(service);
        }
    }

    /**
     * Return the storage engine instance we're using.
     */
    StorageEngine* getStorageEngine() {
        return _storageEngine.get();
    }

    /**
     * Clear the current storage engine so we can set a new one.  This is safe to call only if
     * the caller has arranged for no opCtxs to be accessing the existing storage engine,
     * and that no new opCtxs can be created which will access storage until this call returns.
     *
     * See StorageEngineChangeContext for one way this may be done.
     */
    void clearStorageEngine() {
        _storageEngine = nullptr;
    }

    using StorageChangeMutexType = WriteRarelyRWMutex;

    StorageChangeMutexType& getStorageChangeMutex() {
        return _storageChangeMutex;
    }

    //
    // Global operation management.  This may not belong here and there may be too many methods
    // here.
    //

    /**
     * Signal all OperationContext(s) that they have been killed except the ones belonging to the
     * excluded clients.
     */
    void setKillAllOperations(std::function<bool(const StringData)> excludedClientPredicate = {});

    /**
     * Reset the operation kill state after a killAllOperations.
     * Used for testing.
     */
    void unsetKillAllOperations();

    /**
     * Get the state for killing all operations.
     */
    bool getKillAllOperations() {
        return _globalKill.loadRelaxed();
    }

    /**
     * Kills the operation "opCtx" with the code "killCode", if opCtx has not already been killed.
     * Caller must own the lock on opCtx->getClient, and opCtx->getServiceContext() must be the same
     * as this service context. WithLock expects that the client lock be passed in.
     **/
    void killOperation(ClientLock& clientLock,
                       OperationContext* opCtx,
                       ErrorCodes::Error killCode = ErrorCodes::Interrupted);

    /**
     * Delists the operation by removing it from its client. Both
     * "opCtx->getClient()->getServiceContext()" and "this" must point to the same instance of
     * ServiceContext. Also, "opCtx" should never be deleted before this method returns. Finally,
     * the thread invoking this method must not hold the client and the service context locks.
     */
    void delistOperation(OperationContext* opCtx);

    /**
     * Kills the operation "opCtx" with the code "killCode", if opCtx has not already been killed,
     * and delists the operation by removing it from its client. Both
     * "opCtx->getClient()->getServiceContext()" and "this" must point to the same instance of
     * service context. Also, "opCtx" should never be deleted before this method returns. Finally,
     * the thread invoking this method must not hold (own) the client and the service context locks.
     * It is highly recommended to use "ErrorCodes::OperationIsKilledAndDelisted" as the error code
     * to facilitate debugging.
     */
    void killAndDelistOperation(
        OperationContext* opCtx,
        ErrorCodes::Error killError = ErrorCodes::OperationIsKilledAndDelisted);

    /**
     * Registers a listener to be notified each time an op is killed.
     *
     * listener does not become owned by the environment. As there is currently no way to
     * unregister, the listener object must outlive this ServiceContext object.
     */
    void registerKillOpListener(KillOpListenerInterface* listener);

    //
    // Background tasks.
    //

    /**
     * Set a periodic runner on the service context. The runner should already be
     * started when it is moved onto the service context. The service context merely
     * takes ownership of this object to allow it to continue running for the life of
     * the process
     */
    void setPeriodicRunner(std::unique_ptr<PeriodicRunner> runner);

    /**
     * Returns a pointer to the global periodic runner owned by this service context.
     */
    PeriodicRunner* getPeriodicRunner() const;

    //
    // Transport.
    //

    /**
     * Get the master TransportLayerManager. Routes to all other TransportLayers that
     * may be in use within this service.
     *
     * See TransportLayerManager for more details.
     */
    transport::TransportLayerManager* getTransportLayerManager() const;

    /**
     * Waits for the ServiceContext to be fully initialized and for all TransportLayers to have been
     * added/started.
     *
     * If startup is already complete this returns immediately.
     */
    void waitForStartupComplete();

    /*
     * Marks initialization as complete and all transport layers as started.
     */
    void notifyStorageStartupRecoveryComplete();

    /**
     * Set the OpObserver.
     */
    void setOpObserver(std::unique_ptr<OpObserver> opObserver);

    /**
     * Set the OpObserver, even if that means overwriting an existing one.
     */
    void resetOpObserver_forTest(std::unique_ptr<OpObserver> opObserver);

    /**
     * Return the OpObserver instance we're using. This may be an OpObserverRegistry that in fact
     * contains multiple observers.
     */
    OpObserver* getOpObserver() const {
        return _opObserver.get();
    }

    /**
     * Returns the tick/clock source set in this context.
     */
    TickSource* getTickSource() const {
        return _tickSource.get();
    }

    /**
     * Get a ClockSource implementation that may be less precise than the _preciseClockSource but
     * may be cheaper to call.
     */
    ClockSource* getFastClockSource() const {
        return _fastClockSource.get();
    }

    /**
     * Get a ClockSource implementation that is very precise but may be expensive to call.
     */
    ClockSource* getPreciseClockSource() const {
        return _preciseClockSource.get();
    }

    /**
     * Binds the TransportLayerManager to the service context. The TransportLayerManager should have
     * already had setup() called successfully, but not startup().
     *
     * This should be a TransportLayerManager created with the global server configuration.
     */
    void setTransportLayerManager(std::unique_ptr<transport::TransportLayerManager> tl);

    /**
     * Creates a delayed execution baton with basic functionality
     */
    BatonHandle makeBaton(OperationContext* opCtx) const;

    void disallowUserWrites() {
        _userWritesAllowed.store(false);
    }

    /**
     * Returns true if user writes are allowed.
     *
     * User writes are disallowed when starting with queryableBackupMode or
     * recoverFromOplogAsStandalone to prevent users from writing to replicated collections in
     * standalone mode.
     *
     * To determine whether an operation must run in read-only mode, use
     * OperationContext::readOnly().
     */
    bool userWritesAllowed() const {
        return _userWritesAllowed.load();
    }

    ClientLock getLockedClient(OperationId id);

    /** The `role` must be ShardServer or RouterServer exactly. */
    Service* getService(ClusterRole role) const;

    /**
     * Returns the shard service if it exists.
     * Otherwise, returns the router service.
     *
     * Gets the "main service" of this ServiceContext. Used when a caller needs
     * some Service, but it doesn't matter which
     * Service they get.
     */
    Service* getService() const;

    ClientList::Cursor makeClientsCursor() const {
        return _clientsList.getCursor();
    }

    /**
     * This is for internal use by `ServiceContextLock`. Avoid using it directly to lock
     * `ServiceContext` as it will block normal server operations.
     */
    friend auto makeLockHandleForObjectLock(ServiceContext* svcCtx) {
        return stdx::unique_lock(svcCtx->_mutex);
    }

private:
    class ClientObserverHolder {
    public:
        explicit ClientObserverHolder(std::unique_ptr<ClientObserver> observer)
            : _observer(std::move(observer)) {}
        void onCreate(Client* client) const {
            _observer->onCreateClient(client);
        }
        void onDestroy(Client* client) const {
            _observer->onDestroyClient(client);
        }
        void onCreate(OperationContext* opCtx) const {
            _observer->onCreateOperationContext(opCtx);
        }
        void onDestroy(OperationContext* opCtx) const {
            _observer->onDestroyOperationContext(opCtx);
        }

    private:
        std::unique_ptr<ClientObserver> _observer;
    };

    struct ServiceSet;

    /**
     * Removes the operation from its client. It will acquire both client and service context locks,
     * and should only be used internally by other ServiceContext methods. To ensure delisted
     * operations are shortly deleted, this method should only be called after killing an operation
     * or in its destructor.
     */
    void _delistOperation(OperationContext* opCtx);

    ObservableMutex<stdx::mutex> _mutex;

    /**
     * The periodic runner.
     */
    SyncUnique<PeriodicRunner> _runner;

    /**
     * The TransportLayer.
     */
    SyncUnique<transport::TransportLayerManager> _transportLayerManager;

    /**
     * The storage engine, if any.
     */
    SyncUnique<StorageEngine> _storageEngine;

    /**
     * The mutex that protects changing out the storage engine.
     */
    StorageChangeMutexType _storageChangeMutex;

    /**
     * Vector of registered observers.
     */
    std::vector<ClientObserverHolder> _clientObservers;
    ClientMap _clients;
    ClientList _clientsList;

    /**
     * The registered OpObserver.
     */
    SyncUnique<OpObserver> _opObserver;

    SyncUnique<TickSource> _tickSource;

    /**
     * A ClockSource implementation that may be less precise than the _preciseClockSource but
     * may be cheaper to call.
     */
    SyncUnique<ClockSource> _fastClockSource;


    /**
     * A ClockSource implementation that is very precise but may be expensive to call.
     */
    SyncUnique<ClockSource> _preciseClockSource;

    // Flag set to indicate that all operations are to be interrupted ASAP.
    AtomicWord<bool> _globalKill{false};

    // protected by _mutex
    std::vector<KillOpListenerInterface*> _killOpListeners;

    // Server-wide flag indicating whether users' writes are allowed.
    AtomicWord<bool> _userWritesAllowed{true};

    bool _startupComplete = false;
    stdx::condition_variable _startupCompleteCondVar;

    std::unique_ptr<ServiceSet> _serviceSet;
};

/**
 * A Service is a grouping of Clients, and is a creator of Client objects.
 * It determines the ClusterRole of the Clients and the CommandRegistry
 * available to them. Each service tracks some metrics separately.
 *
 * A Service is logically on a level below the ServiceContext, which holds state
 * for the whole process, and above Client, which holds state for each
 * connection. A ServiceContext owns one or more Service objects.
 *
 * A Service will be the handler for either the "shard" or "router" service, as
 * both services can now exist in the same server process (ServiceContext).
 */
class Service : public Decorable<Service> {
    template <typename T>
    using SyncUnique = service_context_detail::SyncUnique<T>;

public:
    /**
     * Special deleter used for cleaning up Service objects.
     * See UniqueService, below.
     */
    class ServiceDeleter {
    public:
        void operator()(Service* service) const;
    };

    using UniqueService = std::unique_ptr<Service, ServiceDeleter>;
    using ConstructorActionRegisterer = ConstructorActionRegistererType<Service>;

    ~Service() override;

    /**
     * Creates a new Client object representing a client session associated with this
     * Service.
     *
     * The "desc" string is used to set a descriptive name for the client, used in logging.
     *
     * If supplied, "session" is the transport::Session used for communicating with the client.
     */
    ServiceContext::UniqueClient makeClient(
        std::string desc,
        std::shared_ptr<transport::Session> session = nullptr,
        ClientOperationKillableByStepdown killable = ClientOperationKillableByStepdown{true}) {
        return _sc->makeClientForService(std::move(desc), std::move(session), killable, this);
    }

    static UniqueService make(ServiceContext* sc, ClusterRole role);

    ClusterRole role() const {
        return _role;
    }

    ServiceContext* getServiceContext() const {
        return _sc;
    }

    void setServiceEntryPoint(std::unique_ptr<ServiceEntryPoint> sep);

    ServiceEntryPoint* getServiceEntryPoint() const {
        return _serviceEntryPoint.get();
    }

    /**
     * Cursor for enumerating the live Client objects belonging to a Service.
     *
     * Lifetimes of this type are synchronized with client creation and destruction.
     */
    class LockedClientsCursor {
    public:
        explicit LockedClientsCursor(Service* service)
            : _service(service), _cursor(service->getServiceContext()) {}

        /**
         * Returns the next client in the enumeration, or nullptr if there are no more clients.
         */
        ClientLock next();

    private:
        Service* _service;
        ServiceContext::LockedClientsCursor _cursor;
    };

private:
    /**
     * Private constructor. If intending to make a Service object, use the
     * make function instead defined below.
     */
    Service(ServiceContext* sc, ClusterRole role);

    ServiceContext* _sc;
    ClusterRole _role;
    SyncUnique<ServiceEntryPoint> _serviceEntryPoint;
};

/**
 * Returns true if there is a global ServiceContext.
 */
bool hasGlobalServiceContext();

/**
 * Returns the singleton ServiceContext for this server process.
 *
 * Fatal if there is currently no global ServiceContext.
 *
 * Caller does not own pointer.
 */
ServiceContext* getGlobalServiceContext();

/**
 * Returns the ServiceContext associated with the current Client.
 *
 * Returns a nullptr if there is not a current Client
 *
 * Caller does not own pointer.
 */
ServiceContext* getCurrentServiceContext();

/**
 * Sets the global ServiceContext.  If 'serviceContext' is NULL, un-sets and deletes
 * the current global ServiceContext.
 *
 * Takes ownership of 'serviceContext'.
 */
void setGlobalServiceContext(ServiceContext::UniqueServiceContext&& serviceContext);

/**
 * Maps `service`'s ClusterRole (or ClusterRole::None if `service` is nullptr) to a LogService.
 */
logv2::LogService toLogService(Service* service);

}  // namespace mongo
