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

#include <boost/scoped_ptr.hpp>
#include <vector>

#include "mongo/db/service_context.h"
#include "mongo/platform/unordered_set.h"

namespace mongo {

    class Client;
    class StorageEngineLockFile;

    class ServiceContextMongoD : public ServiceContext {
    public:
        typedef std::map<std::string, const StorageEngine::Factory*> FactoryMap;

        ServiceContextMongoD();

        ~ServiceContextMongoD();

        StorageEngine* getGlobalStorageEngine();

        void initializeGlobalStorageEngine();

        void shutdownGlobalStorageEngineCleanly();

        void registerStorageEngine(const std::string& name,
                                   const StorageEngine::Factory* factory);

        bool isRegisteredStorageEngine(const std::string& name);

        StorageFactoriesIterator* makeStorageFactoriesIterator();

        void setKillAllOperations();

        void unsetKillAllOperations();

        bool getKillAllOperations();

        bool killOperation(unsigned int opId);

        void killAllUserOperations(const OperationContext* txn);

        void registerKillOpListener(KillOpListenerInterface* listener);

        OperationContext* newOpCtx();

        void setOpObserver(std::unique_ptr<OpObserver> opObserver);

        OpObserver* getOpObserver();

    private:

        /**
         * Kills the active operation on "client" if that operation is associated with operation id
         * "opId".
         *
         * Returns true if an operation was killed.
         *
         * Must only be called by a thread owning both this service context's mutex and the
         * client's.
         */
        bool _killOperationsAssociatedWithClientAndOpId_inlock(Client* client, unsigned int opId);

        /**
         * Kills the given operation.
         *
         * Caller must own the service context's _mutex.
         */
        void _killOperation_inlock(OperationContext* opCtx);

        bool _globalKill;

        // protected by parent class's _mutex
        std::vector<KillOpListenerInterface*> _killOpListeners;

        boost::scoped_ptr<StorageEngineLockFile> _lockFile;

        // logically owned here, but never deleted by anyone.
        StorageEngine* _storageEngine;

        // logically owned here.
        std::unique_ptr<OpObserver> _opObserver;

        // All possible storage engines are registered here through MONGO_INIT.
        FactoryMap _storageFactories;
    };

    class StorageFactoriesIteratorMongoD : public StorageFactoriesIterator {
    public:

        typedef ServiceContextMongoD::FactoryMap::const_iterator FactoryMapIterator;
        StorageFactoriesIteratorMongoD(const FactoryMapIterator& begin,
                                       const FactoryMapIterator& end);

        virtual bool more() const;
        virtual const StorageEngine::Factory* next();

    private:
        FactoryMapIterator _curr;
        FactoryMapIterator _end;
    };

}  // namespace mongo
