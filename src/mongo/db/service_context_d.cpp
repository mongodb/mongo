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

#include "mongo/platform/basic.h"

#include "mongo/db/service_context_d.h"

#include <boost/optional.hpp>

#include "mongo/base/init.h"
#include "mongo/base/initializer.h"
#include "mongo/db/client.h"
#include "mongo/db/curop.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/operation_context_impl.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/storage_engine_lock_file.h"
#include "mongo/db/storage/storage_engine_metadata.h"
#include "mongo/db/storage_options.h"
#include "mongo/scripting/engine.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/log.h"
#include "mongo/util/map_util.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

    MONGO_INITIALIZER(SetGlobalEnvironment)(InitializerContext* context) {
        setGlobalServiceContext(stdx::make_unique<ServiceContextMongoD>());
        return Status::OK();
    }

    ServiceContextMongoD::ServiceContextMongoD()
        : _globalKill(false),
          _storageEngine(NULL) { }

    ServiceContextMongoD::~ServiceContextMongoD() {

    }

    StorageEngine* ServiceContextMongoD::getGlobalStorageEngine() {
        // We don't check that globalStorageEngine is not-NULL here intentionally.  We can encounter
        // an error before it's initialized and proceed to exitCleanly which is equipped to deal
        // with a NULL storage engine.
        return _storageEngine;
    }

    extern bool _supportsDocLocking;

    void ServiceContextMongoD::initializeGlobalStorageEngine() {
        // This should be set once.
        invariant(!_storageEngine);

        const std::string dbpath = storageGlobalParams.dbpath;
        if (auto existingStorageEngine = StorageEngineMetadata::getStorageEngineForPath(dbpath)) {
            if (storageGlobalParams.engineSetByUser) {
                // Verify that the name of the user-supplied storage engine matches the contents of
                // the metadata file.
                const StorageEngine::Factory* factory = mapFindWithDefault(
                    _storageFactories,
                    storageGlobalParams.engine,
                    static_cast<const StorageEngine::Factory*>(nullptr));

                if (factory) {
                    uassert(28661, str::stream()
                        << "Cannot start server. Detected data files in " << dbpath << " created by"
                        << " the '" << *existingStorageEngine << "' storage engine, but the"
                        << " specified storage engine was '" << factory->getCanonicalName() << "'.",
                        factory->getCanonicalName() == *existingStorageEngine);
                }
            }
            else {
                // Otherwise set the active storage engine as the contents of the metadata file.
                log() << "Detected data files in " << dbpath << " created by the '"
                      << *existingStorageEngine << "' storage engine, so setting the active"
                      << " storage engine to '" << *existingStorageEngine << "'.";
                storageGlobalParams.engine = *existingStorageEngine;
            }
        }
        else if (!storageGlobalParams.engineSetByUser) {
            // Ensure the default storage engine is available with this build of mongod.
            uassert(28662, str::stream()
                << "Cannot start server. The default storage engine '" << storageGlobalParams.engine
                << "' is not available with this build of mongod. Please specify a different"
                << " storage engine explicitly, e.g. --storageEngine=mmapv1.",
                isRegisteredStorageEngine(storageGlobalParams.engine));
        }

        const StorageEngine::Factory* factory = _storageFactories[storageGlobalParams.engine];

        uassert(18656, str::stream()
            << "Cannot start server with an unknown storage engine: " << storageGlobalParams.engine,
            factory);

        std::unique_ptr<StorageEngineMetadata> metadata = StorageEngineMetadata::forPath(dbpath);

        // Validate options in metadata against current startup options.
        if (metadata.get()) {
            uassertStatusOK(factory->validateMetadata(*metadata, storageGlobalParams));
        }

        try {
            _lockFile.reset(new StorageEngineLockFile(storageGlobalParams.dbpath));
        }
        catch (const std::exception& ex) {
            uassert(28596, str::stream()
                << "Unable to determine status of lock file in the data directory "
                << storageGlobalParams.dbpath << ": " << ex.what(),
                false);
        }
        if (_lockFile->createdByUncleanShutdown()) {
            warning() << "Detected unclean shutdown - "
                      << _lockFile->getFilespec() << " is not empty.";
        }
        uassertStatusOK(_lockFile->open());

        ScopeGuard guard = MakeGuard(&StorageEngineLockFile::close, _lockFile.get());
        _storageEngine = factory->create(storageGlobalParams, *_lockFile);
        _storageEngine->finishInit();
        uassertStatusOK(_lockFile->writePid());

        // Write a new metadata file if it is not present.
        if (!metadata.get()) {
            metadata.reset(new StorageEngineMetadata(storageGlobalParams.dbpath));
            metadata->setStorageEngine(factory->getCanonicalName().toString());
            metadata->setStorageEngineOptions(factory->createMetadataOptions(storageGlobalParams));
            uassertStatusOK(metadata->write());
        }

        guard.Dismiss();

        _supportsDocLocking = _storageEngine->supportsDocLocking();
    }

    void ServiceContextMongoD::shutdownGlobalStorageEngineCleanly() {
        invariant(_storageEngine);
        invariant(_lockFile.get());
        _storageEngine->cleanShutdown();
        _lockFile->clearPidAndUnlock();
    }

    void ServiceContextMongoD::registerStorageEngine(const std::string& name,
                                                     const StorageEngine::Factory* factory) {
        // No double-registering.
        invariant(0 == _storageFactories.count(name));

        // Some sanity checks: the factory must exist,
        invariant(factory);

        // and all factories should be added before we pick a storage engine.
        invariant(NULL == _storageEngine);

        _storageFactories[name] = factory;
    }

    bool ServiceContextMongoD::isRegisteredStorageEngine(const std::string& name) {
        return _storageFactories.count(name);
    }

    StorageFactoriesIterator* ServiceContextMongoD::makeStorageFactoriesIterator() {
        return new StorageFactoriesIteratorMongoD(_storageFactories.begin(),
                                                  _storageFactories.end());
    }

    StorageFactoriesIteratorMongoD::StorageFactoriesIteratorMongoD(
        const ServiceContextMongoD::FactoryMap::const_iterator& begin,
        const ServiceContextMongoD::FactoryMap::const_iterator& end) :
        _curr(begin), _end(end) {
    }

    bool StorageFactoriesIteratorMongoD::more() const {
        return _curr != _end;
    }

    const StorageEngine::Factory* StorageFactoriesIteratorMongoD::next() {
        return _curr++->second;
    }

    void ServiceContextMongoD::setKillAllOperations() {
        boost::lock_guard<boost::mutex> clientLock(_mutex);
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

    bool ServiceContextMongoD::getKillAllOperations() {
        return _globalKill;
    }

    bool ServiceContextMongoD::_killOperationsAssociatedWithClientAndOpId_inlock(
            Client* client, unsigned int opId) {
        for( CurOp *k = CurOp::get(client); k; k = k->parent() ) {
            if ( k->opNum() != opId )
                continue;

            k->kill();
            for( CurOp *l = CurOp::get(client); l; l = l->parent() ) {
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

    bool ServiceContextMongoD::killOperation(unsigned int opId) {
        for (LockedClientsCursor cursor(this); Client* client = cursor.next();) {
            bool found = _killOperationsAssociatedWithClientAndOpId_inlock(client, opId);
            if (found) {
                return true;
            }
        }

        return false;
    }

    void ServiceContextMongoD::killAllUserOperations(const OperationContext* txn) {
        for (LockedClientsCursor cursor(this); Client* client = cursor.next();) {
            if (!client->isFromUserConnection()) {
                // Don't kill system operations.
                continue;
            }

            if (CurOp::get(client)->opNum() == txn->getOpID()) {
                // Don't kill ourself.
                continue;
            }

            bool found = _killOperationsAssociatedWithClientAndOpId_inlock(
                    client, CurOp::get(client)->opNum());
            if (!found) {
                warning() << "Attempted to kill operation " << CurOp::get(client)->opNum()
                          << " but the opId changed";
            }
        }
    }

    void ServiceContextMongoD::unsetKillAllOperations() {
        _globalKill = false;
    }

    void ServiceContextMongoD::registerKillOpListener(KillOpListenerInterface* listener) {
        boost::lock_guard<boost::mutex> clientLock(_mutex);
        _killOpListeners.push_back(listener);
    }

    OperationContext* ServiceContextMongoD::newOpCtx() {
        return new OperationContextImpl();
    }

    void ServiceContextMongoD::setOpObserver(std::unique_ptr<OpObserver> opObserver) {
        _opObserver.reset(opObserver.get());
    }

    OpObserver* ServiceContextMongoD::getOpObserver() {
        return _opObserver.get();
    }

}  // namespace mongo
