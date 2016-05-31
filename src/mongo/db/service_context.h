/**
 *    Copyright (C) 2014 MongoDB Inc.
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

#include <memory>
#include <vector>

#include "mongo/base/disallow_copying.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/unordered_set.h"
#include "mongo/stdx/functional.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/decorable.h"
#include "mongo/util/tick_source.h"

namespace mongo {

class AbstractMessagingPort;
class Client;
class OperationContext;
class OpObserver;

namespace transport {
class Session;
class TransportLayer;
class TransportLayerManager;
}  // namespace transport

/**
 * Classes that implement this interface can receive notification on killOp.
 *
 * See registerKillOpListener() for more information,
 * including limitations on the lifetime of registered listeners.
 */
class KillOpListenerInterface {
public:
    /**
     * Will be called *after* ops have been told they should die.
     * Callback must not fail.
     */
    virtual void interrupt(unsigned opId) = 0;
    virtual void interruptAll() = 0;

protected:
    // Should not delete through a pointer of this type
    virtual ~KillOpListenerInterface() {}
};

class StorageFactoriesIterator {
    MONGO_DISALLOW_COPYING(StorageFactoriesIterator);

public:
    virtual ~StorageFactoriesIterator() {}
    virtual bool more() const = 0;
    virtual const StorageEngine::Factory* next() = 0;

protected:
    StorageFactoriesIterator() {}
};

/**
 * Class representing the context of a service, such as a MongoD database service or
 * a MongoS routing service.
 *
 * A ServiceContext is the root of a hierarchy of contexts.  A ServiceContext owns
 * zero or more Clients, which in turn each own OperationContexts.
 */
class ServiceContext : public Decorable<ServiceContext> {
    MONGO_DISALLOW_COPYING(ServiceContext);

public:
    /**
     * Special deleter used for cleaning up Client objects owned by a ServiceContext.
     * See UniqueClient, below.
     */
    class ClientDeleter {
    public:
        void operator()(Client* client) const;
    };

    /**
     * Observer interface implemented to hook client and operation context creation and
     * destruction.
     */
    class ClientObserver {
    public:
        virtual ~ClientObserver() = default;

        /**
         * Hook called after a new client "client" is created on a service by
         * service->makeClient().
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
         *
         * Like a destructor, must not throw exceptions.
         */
        virtual void onDestroyOperationContext(OperationContext* opCtx) = 0;
    };

    using ClientSet = unordered_set<Client*>;

    /**
     * Cursor for enumerating the live Client objects belonging to a ServiceContext.
     *
     * Lifetimes of this type are synchronized with client creation and destruction.
     */
    class LockedClientsCursor {
    public:
        /**
         * Constructs a cursor for enumerating the clients of "service", blocking "service" from
         * creating or destroying Client objects until this instance is destroyed.
         */
        explicit LockedClientsCursor(ServiceContext* service);

        /**
         * Returns the next client in the enumeration, or nullptr if there are no more clients.
         */
        Client* next();

    private:
        stdx::unique_lock<stdx::mutex> _lock;
        ClientSet::const_iterator _curr;
        ClientSet::const_iterator _end;
    };

    /**
     * Special deleter used for cleaning up OperationContext objects owned by a ServiceContext.
     * See UniqueOperationContext, below.
     */
    class OperationContextDeleter {
    public:
        void operator()(OperationContext* opCtx) const;
    };

    /**
     * This is the unique handle type for Clients created by a ServiceContext.
     */
    using UniqueClient = std::unique_ptr<Client, ClientDeleter>;

    /**
     * This is the unique handle type for OperationContexts created by a ServiceContext.
     */
    using UniqueOperationContext = std::unique_ptr<OperationContext, OperationContextDeleter>;

    virtual ~ServiceContext();

    /**
     * Registers an observer of lifecycle events on Clients created by this ServiceContext.
     *
     * See the ClientObserver type, above, for details.
     *
     * All calls to registerClientObserver must complete before ServiceContext
     * is used in multi-threaded operation, or is used to create clients via calls
     * to makeClient.
     */
    void registerClientObserver(std::unique_ptr<ClientObserver> observer);

    /**
     * Creates a new Client object representing a client session associated with this
     * ServiceContext.
     *
     * The "desc" string is used to set a descriptive name for the client, used in logging.
     *
     * If supplied, "session" is the transport::Session used for communicating with the client.
     */
    UniqueClient makeClient(std::string desc, transport::Session* session = nullptr);

    /**
     * Creates a new OperationContext on "client".
     *
     * "client" must not have an active operation context.
     */
    UniqueOperationContext makeOperationContext(Client* client);

    //
    // Storage
    //

    /**
     * Register a storage engine.  Called from a MONGO_INIT that depends on initializiation of
     * the global environment.
     * Ownership of 'factory' is transferred to global environment upon registration.
     */
    virtual void registerStorageEngine(const std::string& name,
                                       const StorageEngine::Factory* factory) = 0;

    /**
     * Returns true if "name" refers to a registered storage engine.
     */
    virtual bool isRegisteredStorageEngine(const std::string& name) = 0;

    /**
     * Produce an iterator over all registered storage engine factories.
     * Caller owns the returned object and is responsible for deleting when finished.
     *
     * Never returns nullptr.
     */
    virtual StorageFactoriesIterator* makeStorageFactoriesIterator() = 0;

    virtual void initializeGlobalStorageEngine() = 0;

    /**
     * Shuts down storage engine cleanly and releases any locks on mongod.lock.
     */
    virtual void shutdownGlobalStorageEngineCleanly() = 0;

    /**
     * Return the storage engine instance we're using.
     */
    virtual StorageEngine* getGlobalStorageEngine() = 0;

    //
    // Global operation management.  This may not belong here and there may be too many methods
    // here.
    //

    /**
     * Signal all OperationContext(s) that they have been killed.
     */
    void setKillAllOperations();

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
     * @param i opid of operation to kill
     * @return if operation was found
     **/
    bool killOperation(unsigned int opId);

    /**
     * Kills all operations that have a Client that is associated with an incoming user
     * connection, except for the one associated with txn.
     */
    void killAllUserOperations(const OperationContext* txn, ErrorCodes::Error killCode);

    /**
     * Registers a listener to be notified each time an op is killed.
     *
     * listener does not become owned by the environment. As there is currently no way to
     * unregister, the listener object must outlive this ServiceContext object.
     */
    void registerKillOpListener(KillOpListenerInterface* listener);

    //
    // Transport.
    //

    /**
     * Get the master TransportLayer. Routes to all other TransportLayers that
     * may be in use within this service.
     *
     * See TransportLayerManager for more details.
     */
    transport::TransportLayer* getTransportLayer() const;

    /**
     * Add a new TransportLayer to this service context. The new TransportLayer will
     * be added to the TransportLayerManager accessible via getTransportLayer().
     *
     * It additionally calls start() on the TransportLayer after adding it.
     */
    Status addAndStartTransportLayer(std::unique_ptr<transport::TransportLayer> tl);

    //
    // Global OpObserver.
    //

    /**
     * Set the OpObserver.
     */
    virtual void setOpObserver(std::unique_ptr<OpObserver> opObserver) = 0;

    /**
     * Return the OpObserver instance we're using.
     */
    virtual OpObserver* getOpObserver() = 0;

    /**
     * Returns the tick/clock source set in this context.
     */
    TickSource* getTickSource() const;

    /**
     * Get a ClockSource implementation that may be less precise than the _preciseClockSource but
     * may be cheaper to call.
     */
    ClockSource* getFastClockSource() const;

    /**
     * Get a ClockSource implementation that is very precise but may be expensive to call.
     */
    ClockSource* getPreciseClockSource() const;

    /**
     * Replaces the current tick/clock source with a new one. In other words, the old source will be
     * destroyed. So make sure that no one is using the old source when calling this.
     */
    void setTickSource(std::unique_ptr<TickSource> newSource);

    /**
     * Call this method with a ClockSource implementation that may be less precise than
     * the _preciseClockSource but may be cheaper to call.
     */
    void setFastClockSource(std::unique_ptr<ClockSource> newSource);

    /**
     * Call this method with a ClockSource implementation that is very precise but
     * may be expensive to call.
     */
    void setPreciseClockSource(std::unique_ptr<ClockSource> newSource);

protected:
    ServiceContext();

    /**
     * Mutex used to synchronize access to mutable state of this ServiceContext instance,
     * including possibly by its subclasses.
     */
    stdx::mutex _mutex;

private:
    /**
     * Returns a new OperationContext. Private, for use by makeOperationContext.
     */
    virtual std::unique_ptr<OperationContext> _newOpCtx(Client* client, unsigned opId) = 0;

    /**
     * Kills the given operation.
     *
     * Caller must own the service context's _mutex.
     */
    void _killOperation_inlock(OperationContext* opCtx, ErrorCodes::Error killCode);


    /**
     * The TransportLayerManager.
     */
    std::unique_ptr<transport::TransportLayerManager> _transportLayerManager;

    /**
     * Vector of registered observers.
     */
    std::vector<std::unique_ptr<ClientObserver>> _clientObservers;
    ClientSet _clients;

    std::unique_ptr<TickSource> _tickSource;

    /**
     * A ClockSource implementation that may be less precise than the _preciseClockSource but
     * may be cheaper to call.
     */
    std::unique_ptr<ClockSource> _fastClockSource;

    /**
     * A ClockSource implementation that is very precise but may be expensive to call.
     */
    std::unique_ptr<ClockSource> _preciseClockSource;

    // Flag set to indicate that all operations are to be interrupted ASAP.
    AtomicWord<bool> _globalKill{false};

    // protected by _mutex
    std::vector<KillOpListenerInterface*> _killOpListeners;

    // Counter for assigning operation ids.
    AtomicUInt32 _nextOpId{1};
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
 * Sets the global ServiceContext.  If 'serviceContext' is NULL, un-sets and deletes
 * the current global ServiceContext.
 *
 * Takes ownership of 'serviceContext'.
 */
void setGlobalServiceContext(std::unique_ptr<ServiceContext>&& serviceContext);

/**
 * Shortcut for querying the storage engine about whether it supports document-level locking.
 * If this call becomes too expensive, we could cache the value somewhere so we don't have to
 * fetch the storage engine every time.
 */
bool supportsDocLocking();

/**
 * Returns true if the storage engine in use is MMAPV1.
 */
bool isMMAPV1();

/*
 * Extracts the storageEngine bson from the CollectionOptions provided.  Loops through each
 * provided storageEngine and asks the matching registered storage engine if the
 * collection/index options are valid.  Returns an error if the collection/index options are
 * invalid.
 * If no matching registered storage engine is found, return an error.
 * Validation function 'func' must be either:
 * - &StorageEngine::Factory::validateCollectionStorageOptions; or
 * - &StorageEngine::Factory::validateIndexStorageOptions
 */
Status validateStorageOptions(
    const BSONObj& storageEngineOptions,
    stdx::function<Status(const StorageEngine::Factory* const, const BSONObj&)> validateFunc);

/*
 * Returns a BSONArray containing the names of available storage engines, or an empty
 * array if there is no global ServiceContext
 */
BSONArray storageEngineList();

/*
 * Appends a the list of available storage engines to a BSONObjBuilder for reporting purposes.
 */
void appendStorageEngineList(BSONObjBuilder* result);

}  // namespace mongo
