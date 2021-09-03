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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "mongo/db/storage/storage_engine_init.h"

#include <map>
#include <memory>

#include "mongo/base/init.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/concurrency/lock_state.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/control/storage_control.h"
#include "mongo/db/storage/recovery_unit_noop.h"
#include "mongo/db/storage/storage_engine_change_context.h"
#include "mongo/db/storage/storage_engine_lock_file.h"
#include "mongo/db/storage/storage_engine_metadata.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/storage/storage_repair_observer.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

namespace mongo {

namespace {
/**
 * Creates the lock file used to prevent concurrent processes from accessing the data files,
 * as appropriate.
 */
void createLockFile(ServiceContext* service);
}  // namespace

StorageEngine::LastShutdownState initializeStorageEngine(OperationContext* opCtx,
                                                         const StorageEngineInitFlags initFlags) {
    ServiceContext* service = opCtx->getServiceContext();

    // This should be set once.
    if ((initFlags & StorageEngineInitFlags::kForRestart) == StorageEngineInitFlags{})
        invariant(!service->getStorageEngine());

    if ((initFlags & StorageEngineInitFlags::kAllowNoLockFile) == StorageEngineInitFlags{}) {
        createLockFile(service);
    }

    const std::string dbpath = storageGlobalParams.dbpath;

    if (!storageGlobalParams.readOnly) {
        StorageRepairObserver::set(service, std::make_unique<StorageRepairObserver>(dbpath));
        auto repairObserver = StorageRepairObserver::get(service);

        if (storageGlobalParams.repair) {
            repairObserver->onRepairStarted();
        } else if (repairObserver->isIncomplete()) {
            LOGV2_FATAL_NOTRACE(
                50922,
                "An incomplete repair has been detected! This is likely because a repair "
                "operation unexpectedly failed before completing. MongoDB will not start up "
                "again without --repair.");
        }
    }

    if (auto existingStorageEngine = StorageEngineMetadata::getStorageEngineForPath(dbpath)) {
        if (storageGlobalParams.engineSetByUser) {
            // Verify that the name of the user-supplied storage engine matches the contents of
            // the metadata file.
            const StorageEngine::Factory* factory =
                getFactoryForStorageEngine(service, storageGlobalParams.engine);
            if (factory) {
                uassert(28662,
                        str::stream()
                            << "Cannot start server. Detected data files in " << dbpath
                            << " created by"
                            << " the '" << *existingStorageEngine << "' storage engine, but the"
                            << " specified storage engine was '" << factory->getCanonicalName()
                            << "'.",
                        factory->getCanonicalName() == *existingStorageEngine);
            }
        } else {
            // Otherwise set the active storage engine as the contents of the metadata file.
            LOGV2(22270,
                  "Storage engine to use detected by data files",
                  "dbpath"_attr = boost::filesystem::path(dbpath).generic_string(),
                  "storageEngine"_attr = *existingStorageEngine);
            storageGlobalParams.engine = *existingStorageEngine;
        }
    }

    const StorageEngine::Factory* factory =
        getFactoryForStorageEngine(service, storageGlobalParams.engine);

    uassert(18656,
            str::stream() << "Cannot start server with an unknown storage engine: "
                          << storageGlobalParams.engine,
            factory);

    if (storageGlobalParams.readOnly) {
        uassert(34368,
                str::stream()
                    << "Server was started in read-only mode, but the configured storage engine, "
                    << storageGlobalParams.engine << ", does not support read-only operation",
                factory->supportsReadOnly());
    }

    std::unique_ptr<StorageEngineMetadata> metadata;
    if ((initFlags & StorageEngineInitFlags::kSkipMetadataFile) == StorageEngineInitFlags{}) {
        metadata = StorageEngineMetadata::forPath(dbpath);
    }

    if (storageGlobalParams.readOnly) {
        uassert(34415,
                "Server was started in read-only mode, but the storage metadata file was not"
                " found.",
                metadata.get());
    }

    // Validate options in metadata against current startup options.
    if (metadata.get()) {
        uassertStatusOK(factory->validateMetadata(*metadata, storageGlobalParams));
    }

    ScopeGuard guard([&] {
        auto& lockFile = StorageEngineLockFile::get(service);
        if (lockFile) {
            lockFile->close();
        }
    });

    auto& lockFile = StorageEngineLockFile::get(service);
    if ((initFlags & StorageEngineInitFlags::kForRestart) == StorageEngineInitFlags{}) {
        auto storageEngine = std::unique_ptr<StorageEngine>(
            factory->create(opCtx, storageGlobalParams, lockFile ? &*lockFile : nullptr));
        service->setStorageEngine(std::move(storageEngine));
    } else {
        auto storageEngineChangeContext = StorageEngineChangeContext::get(service);
        auto token = storageEngineChangeContext->killOpsForStorageEngineChange(service);
        auto storageEngine = std::unique_ptr<StorageEngine>(
            factory->create(opCtx, storageGlobalParams, lockFile ? &*lockFile : nullptr));
        storageEngineChangeContext->changeStorageEngine(
            service, std::move(token), std::move(storageEngine));
    }
    service->getStorageEngine()->finishInit();

    if (lockFile) {
        uassertStatusOK(lockFile->writePid());
    }

    // Write a new metadata file if it is not present.
    if (!metadata.get() &&
        (initFlags & StorageEngineInitFlags::kSkipMetadataFile) == StorageEngineInitFlags{}) {
        invariant(!storageGlobalParams.readOnly);
        metadata.reset(new StorageEngineMetadata(storageGlobalParams.dbpath));
        metadata->setStorageEngine(factory->getCanonicalName().toString());
        metadata->setStorageEngineOptions(factory->createMetadataOptions(storageGlobalParams));
        uassertStatusOK(metadata->write());
    }

    guard.dismiss();

    if (lockFile && lockFile->createdByUncleanShutdown()) {
        return StorageEngine::LastShutdownState::kUnclean;
    } else {
        return StorageEngine::LastShutdownState::kClean;
    }
}

namespace {
void shutdownGlobalStorageEngineCleanly(ServiceContext* service, Status errorToReport) {
    auto storageEngine = service->getStorageEngine();
    invariant(storageEngine);
    // We always use 'forRestart' = false here because 'forRestart' = true is only appropriate if
    // we're going to restart controls on the same storage engine, which we are not here because
    // we are shutting the storage engine down.
    StorageControl::stopStorageControls(service, errorToReport, /*forRestart=*/false);
    storageEngine->cleanShutdown();
    auto& lockFile = StorageEngineLockFile::get(service);
    if (lockFile) {
        lockFile->clearPidAndUnlock();
        lockFile = boost::none;
    }
}
} /* namespace */

void shutdownGlobalStorageEngineCleanly(ServiceContext* service) {
    shutdownGlobalStorageEngineCleanly(
        service, {ErrorCodes::ShutdownInProgress, "The storage catalog is being closed."});
}

StorageEngine::LastShutdownState reinitializeStorageEngine(OperationContext* opCtx,
                                                           StorageEngineInitFlags initFlags) {
    auto service = opCtx->getServiceContext();
    opCtx->recoveryUnit()->abandonSnapshot();
    shutdownGlobalStorageEngineCleanly(
        service,
        {ErrorCodes::InterruptedDueToStorageChange, "The storage engine is being reinitialized."});
    opCtx->setRecoveryUnit(std::make_unique<RecoveryUnitNoop>(),
                           WriteUnitOfWork::RecoveryUnitState::kNotInUnitOfWork);
    auto lastShutdownState =
        initializeStorageEngine(opCtx, initFlags | StorageEngineInitFlags::kForRestart);
    StorageControl::startStorageControls(service);
    return lastShutdownState;
}

namespace {

void createLockFile(ServiceContext* service) {
    auto& lockFile = StorageEngineLockFile::get(service);
    try {
        lockFile.emplace(storageGlobalParams.dbpath);
    } catch (const std::exception& ex) {
        uassert(28596,
                str::stream() << "Unable to determine status of lock file in the data directory "
                              << storageGlobalParams.dbpath << ": " << ex.what(),
                false);
    }
    const bool wasUnclean = lockFile->createdByUncleanShutdown();
    const auto openStatus = lockFile->open();
    if (storageGlobalParams.readOnly && openStatus == ErrorCodes::IllegalOperation) {
        lockFile = boost::none;
    } else {
        uassertStatusOK(openStatus);
    }

    if (wasUnclean) {
        if (storageGlobalParams.readOnly) {
            LOGV2_FATAL_NOTRACE(34416,
                                "Attempted to open dbpath in readOnly mode, but the server was "
                                "previously not shut down cleanly.");
        }
        LOGV2_WARNING(22271,
                      "Detected unclean shutdown - Lock file is not empty",
                      "lockFile"_attr = lockFile->getFilespec());
    }
}

using FactoryMap = std::map<std::string, std::unique_ptr<StorageEngine::Factory>>;

auto storageFactories = ServiceContext::declareDecoration<FactoryMap>();

}  // namespace

void registerStorageEngine(ServiceContext* service,
                           std::unique_ptr<StorageEngine::Factory> factory) {
    // No double-registering.
    invariant(!getFactoryForStorageEngine(service, factory->getCanonicalName()));

    // Some sanity checks: the factory must exist,
    invariant(factory);

    // and all factories should be added before we pick a storage engine.
    invariant(!service->getStorageEngine());

    auto name = factory->getCanonicalName().toString();
    storageFactories(service).emplace(name, std::move(factory));
}

bool isRegisteredStorageEngine(ServiceContext* service, StringData name) {
    return getFactoryForStorageEngine(service, name);
}

StorageEngine::Factory* getFactoryForStorageEngine(ServiceContext* service, StringData name) {
    const auto result = storageFactories(service).find(name.toString());
    if (result == storageFactories(service).end()) {
        return nullptr;
    }
    return result->second.get();
}

Status validateStorageOptions(
    ServiceContext* service,
    const BSONObj& storageEngineOptions,
    std::function<Status(const StorageEngine::Factory* const, const BSONObj&)> validateFunc) {

    BSONObjIterator storageIt(storageEngineOptions);
    while (storageIt.more()) {
        BSONElement storageElement = storageIt.next();
        StringData storageEngineName = storageElement.fieldNameStringData();
        if (storageElement.type() != mongo::Object) {
            return Status(ErrorCodes::BadValue,
                          str::stream() << "'storageEngine." << storageElement.fieldNameStringData()
                                        << "' has to be an embedded document.");
        }

        if (auto factory = getFactoryForStorageEngine(service, storageEngineName)) {
            Status status = validateFunc(factory, storageElement.Obj());
            if (!status.isOK()) {
                return status;
            }
        } else {
            return Status(ErrorCodes::InvalidOptions,
                          str::stream() << storageEngineName
                                        << " is not a registered storage engine for this server");
        }
    }
    return Status::OK();
}

namespace {
BSONArray storageEngineList(ServiceContext* service) {
    if (!service)
        return BSONArray();

    BSONArrayBuilder engineArrayBuilder;

    for (const auto& nameAndFactory : storageFactories(service)) {
        engineArrayBuilder.append(nameAndFactory.first);
    }

    return engineArrayBuilder.arr();
}
}  // namespace

void appendStorageEngineList(ServiceContext* service, BSONObjBuilder* result) {
    result->append("storageEngines", storageEngineList(service));
}

namespace {

class StorageClientObserver final : public ServiceContext::ClientObserver {
public:
    void onCreateClient(Client* client) override{};
    void onDestroyClient(Client* client) override{};
    void onCreateOperationContext(OperationContext* opCtx) {
        // Use a fully fledged lock manager even when the storage engine is not set.
        opCtx->setLockState(std::make_unique<LockerImpl>());

        auto service = opCtx->getServiceContext();
        // We must prevent storage engine changes while setting the recovery unit on the opCtx.
        auto sharedStorageChangeToken =
            StorageEngineChangeContext::get(service)->acquireSharedStorageChangeToken();

        // There are a few cases where we don't have a storage engine available yet when creating an
        // operation context.
        // 1. During startup, we create an operation context to allow the storage engine
        //    initialization code to make use of the lock manager.
        // 2. There are unit tests that create an operation context before initializing the storage
        //    engine.
        // 3. Unit tests that use an operation context but don't require a storage engine for their
        //    testing purpose.
        auto storageEngine = service->getStorageEngine();
        if (!storageEngine) {
            return;
        }
        opCtx->setRecoveryUnit(std::unique_ptr<RecoveryUnit>(storageEngine->newRecoveryUnit()),
                               WriteUnitOfWork::RecoveryUnitState::kNotInUnitOfWork);
    }
    void onDestroyOperationContext(OperationContext* opCtx) {
        StorageEngineChangeContext::get(opCtx->getServiceContext())
            ->onDestroyOperationContext(opCtx);
    }
};

ServiceContext::ConstructorActionRegisterer registerStorageClientObserverConstructor{
    "RegisterStorageClientObserverConstructor", [](ServiceContext* service) {
        service->registerClientObserver(std::make_unique<StorageClientObserver>());
    }};

}  // namespace
}  // namespace mongo
