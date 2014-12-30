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

#include "mongo/base/disallow_copying.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/stdx/functional.h"

namespace mongo {

    class OperationContext;

    /**
     * Classes that implement this interface can receive notification on killOp.
     *
     * See GlobalEnvironmentExperiment::registerKillOpListener() for more information, including
     * limitations on the lifetime of registered listeners.
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
        virtual ~StorageFactoriesIterator() { }
        virtual bool more() const = 0;
        virtual const StorageEngine::Factory* const & next() = 0;
        virtual const StorageEngine::Factory* const & get() const = 0;
    protected:
        StorageFactoriesIterator() { }
    };

    class GlobalEnvironmentExperiment {
        MONGO_DISALLOW_COPYING(GlobalEnvironmentExperiment);
    public:
        virtual ~GlobalEnvironmentExperiment() { }

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
         */
        virtual StorageFactoriesIterator* makeStorageFactoriesIterator() = 0;

        /**
         * Set the storage engine.  The engine must have been registered via registerStorageEngine.
         */
        virtual void setGlobalStorageEngine(const std::string& name) = 0;

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
        virtual void setKillAllOperations() = 0;

        /**
         * Reset the operation kill state after a killAllOperations.
         * Used for testing.
         */
        virtual void unsetKillAllOperations() = 0;

        /**
         * Get the state for killing all operations.
         */
        virtual bool getKillAllOperations() = 0;

        /**
         * @param i opid of operation to kill
         * @return if operation was found 
         **/
        virtual bool killOperation(unsigned int opId) = 0;

        /**
         * Kills all operations that have a Client that is associated with an incoming user
         * connection, except for the one associated with txn.
         */
        virtual void killAllUserOperations(const OperationContext* txn) = 0;

        /**
         * Registers a listener to be notified each time an op is killed.
         *
         * listener does not become owned by the environment. As there is currently no way to
         * unregister, the listener object must outlive this GlobalEnvironmentExperiment object.
         */
        virtual void registerKillOpListener(KillOpListenerInterface* listener) = 0;

        /**
         * Returns a new OperationContext.  Caller owns pointer.
         */
        virtual OperationContext* newOpCtx() = 0;

    protected:
        GlobalEnvironmentExperiment() { }
    };

    /**
     * Returns true if there is a globalEnvironment.
     */
    bool hasGlobalEnvironment();

    /**
     * Returns the singleton GlobalEnvironmentExperiment for this server process.
     *
     * Fatal if there is currently no globalEnvironment.
     *
     * Caller does not own pointer.
     */
    GlobalEnvironmentExperiment* getGlobalEnvironment();

    /**
     * Sets the GlobalEnvironmentExperiment.  If 'globalEnvironment' is NULL, un-sets and deletes
     * the current GlobalEnvironmentExperiment.
     *
     * Takes ownership of 'globalEnvironment'.
     */
    void setGlobalEnvironment(GlobalEnvironmentExperiment* globalEnvironment);

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
    Status validateStorageOptions(const BSONObj& storageEngineOptions,
        stdx::function<Status (const StorageEngine::Factory* const, const BSONObj&)> validateFunc);

}  // namespace mongo
