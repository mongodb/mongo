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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/db/global_environment_d.h"

#include "mongo/base/init.h"
#include "mongo/base/initializer.h"
#include "mongo/db/client.h"
#include "mongo/db/curop.h"
#include "mongo/db/operation_context_impl.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/storage_engine_metadata.h"
#include "mongo/scripting/engine.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/log.h"

namespace mongo {

    MONGO_INITIALIZER(SetGlobalEnvironment)(InitializerContext* context) {
        setGlobalEnvironment(new GlobalEnvironmentMongoD());
        return Status::OK();
    }

    GlobalEnvironmentMongoD::GlobalEnvironmentMongoD()
        : _globalKill(false),
          _storageEngine(NULL) { }

    GlobalEnvironmentMongoD::~GlobalEnvironmentMongoD() {

    }

    StorageEngine* GlobalEnvironmentMongoD::getGlobalStorageEngine() {
        // We don't check that globalStorageEngine is not-NULL here intentionally.  We can encounter
        // an error before it's initialized and proceed to exitCleanly which is equipped to deal
        // with a NULL storage engine.
        return _storageEngine;
    }

    void GlobalEnvironmentMongoD::setGlobalStorageEngine(const std::string& name) {
        // This should be set once.
        invariant(!_storageEngine);

        const StorageEngine::Factory* factory = _storageFactories[name];

        uassert(18656, str::stream()
            << "Cannot start server with an unknown storage engine: " << name,
            factory);

        std::string canonicalName = factory->getCanonicalName().toString();

        // Do not proceed if data directory has been used by a different storage engine previously.
        StorageEngineMetadata::validate(storageGlobalParams.dbpath, canonicalName);

        _storageEngine = factory->create(storageGlobalParams);
        _storageEngine->finishInit();

        // Write a new metadata file if it is not present.
        StorageEngineMetadata::updateIfMissing(storageGlobalParams.dbpath, canonicalName);
    }

    void GlobalEnvironmentMongoD::registerStorageEngine(const std::string& name,
                                                        const StorageEngine::Factory* factory) {
        // No double-registering.
        invariant(0 == _storageFactories.count(name));

        // Some sanity checks: the factory must exist,
        invariant(factory);

        // and all factories should be added before we pick a storage engine.
        invariant(NULL == _storageEngine);

        _storageFactories[name] = factory;
    }

    bool GlobalEnvironmentMongoD::isRegisteredStorageEngine(const std::string& name) {
        return _storageFactories.count(name);
    }

    StorageFactoriesIterator* GlobalEnvironmentMongoD::makeStorageFactoriesIterator() {
        return new StorageFactoriesIteratorMongoD(_storageFactories.begin(),
                                                  _storageFactories.end());
    }

    StorageFactoriesIteratorMongoD::StorageFactoriesIteratorMongoD(
        const GlobalEnvironmentMongoD::FactoryMap::const_iterator& begin,
        const GlobalEnvironmentMongoD::FactoryMap::const_iterator& end) :
        _curr(begin), _end(end) {
    }


    StorageFactoriesIteratorMongoD::~StorageFactoriesIteratorMongoD() {
    }

    bool StorageFactoriesIteratorMongoD::more() const {
        return _curr != _end;
    }

    const StorageEngine::Factory* const & StorageFactoriesIteratorMongoD::next() {
        return _curr++->second;
    }

    const StorageEngine::Factory* const & StorageFactoriesIteratorMongoD::get() const {
        return _curr->second;
    }

    void GlobalEnvironmentMongoD::setKillAllOperations() {
        boost::mutex::scoped_lock clientLock(Client::clientsMutex);
        _globalKill = true;
        for (size_t i = 0; i < _killOpListeners.size(); i++) {
            try {
                _killOpListeners[i]->interruptAll();
            }
            catch (...) {
                std::terminate();
            }
        }
    }

    bool GlobalEnvironmentMongoD::getKillAllOperations() {
        return _globalKill;
    }

    bool GlobalEnvironmentMongoD::_killOperationsAssociatedWithClientAndOpId_inlock(
            Client* client, unsigned int opId) {
        for( CurOp *k = client->curop(); k; k = k->parent() ) {
            if ( k->opNum() != opId )
                continue;

            k->kill();
            for( CurOp *l = client->curop(); l; l = l->parent() ) {
                l->kill();
            }

            for (size_t i = 0; i < _killOpListeners.size(); i++) {
                try {
                    _killOpListeners[i]->interrupt(opId);
                }
                catch (...) {
                    std::terminate();
                }
            }
            return true;
        }
        return false;
    }

    bool GlobalEnvironmentMongoD::killOperation(unsigned int opId) {
        boost::mutex::scoped_lock clientLock(Client::clientsMutex);

        for(ClientSet::const_iterator j = Client::clients.begin();
                j != Client::clients.end(); ++j) {

            Client* client = *j;

            bool found = _killOperationsAssociatedWithClientAndOpId_inlock(client, opId);
            if (found) {
                return true;
            }
        }

        return false;
    }

    void GlobalEnvironmentMongoD::killAllUserOperations(const OperationContext* txn) {
        boost::mutex::scoped_lock scopedLock(Client::clientsMutex);
        for (ClientSet::const_iterator i = Client::clients.begin();
                i != Client::clients.end(); i++) {

            Client* client = *i;
            if (!client->isFromUserConnection()) {
                // Don't kill system operations.
                continue;
            }

            if (client->curop()->opNum() == txn->getOpID()) {
                // Don't kill ourself.
                continue;
            }

            bool found = _killOperationsAssociatedWithClientAndOpId_inlock(
                    client, client->curop()->opNum());
            invariant(found);
        }
    }

    void GlobalEnvironmentMongoD::unsetKillAllOperations() {
        _globalKill = false;
    }

    void GlobalEnvironmentMongoD::registerKillOpListener(KillOpListenerInterface* listener) {
        boost::mutex::scoped_lock clientLock(Client::clientsMutex);
        _killOpListeners.push_back(listener);
    }

    OperationContext* GlobalEnvironmentMongoD::newOpCtx() {
        return new OperationContextImpl();
    }

}  // namespace mongo
