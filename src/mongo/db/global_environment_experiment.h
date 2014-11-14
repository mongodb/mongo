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
         * Set the storage engine.  The engine must have been registered via registerStorageEngine.
         */
        virtual void setGlobalStorageEngine(const std::string& name) = 0;

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
         * Registers a listener to be notified each time an op is killed.
         *
         * listener does not become owned by the environment. As there is currently no way to
         * unregister, the listener object must outlive this GlobalEnvironmentExperiment object.
         */
        virtual void registerKillOpListener(KillOpListenerInterface* listener) = 0;

        /**
         * Registers the specified operation context on the global environment, so it is
         * discoverable by diagnostics tools.
         *
         * This function must be thread-safe.
         */
        virtual void registerOperationContext(OperationContext* txn) = 0;

        /**
         * Unregisters a previously-registered operation context. It is an error to unregister the
         * same context twice or to unregister a context, which has not previously been registered.
         *
         * This function must be thread-safe.
         */
        virtual void unregisterOperationContext(OperationContext* txn) = 0;

        /**
         * Notification object to be passed to forEachOperationContext so that certain processing
         * can be done on all registered contexts.
         */
        class ProcessOperationContext {
        public:

            /**
             * Invoked for each registered OperationContext. The pointer is guaranteed to be stable
             * until the call returns.
             * 
             * Implementations of this method should not acquire locks or do any operations, which 
             * might block and should generally do as little work as possible in order to not block
             * the iteration or the release of the OperationContext.
             */
            virtual void processOpContext(OperationContext* txn) = 0;

            virtual ~ProcessOperationContext() { }
        };

        /**
         * Iterates over all registered operation contexts and invokes 
         * ProcessOperationContext::processOpContext for each.
         */
        virtual void forEachOperationContext(ProcessOperationContext* procOpCtx) = 0;

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

}  // namespace mongo
