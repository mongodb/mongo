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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "mongo/db/storage/storage_engine_init.h"

#include <map>

#include "mongo/base/init.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/concurrency/lock_state.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/storage_engine_lock_file.h"
#include "mongo/db/storage/storage_engine_metadata.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/storage/storage_repair_observer.h"
#include "mongo/db/unclean_shutdown.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/str.h"

namespace mongo {

namespace {
/**
 * Creates the lock file used to prevent concurrent processes from accessing the data files,
 * as appropriate.
 */
void createLockFile(ServiceContext* service);
}  // namespace

extern bool _supportsDocLocking;

void initializeStorageEngine(ServiceContext* service, const StorageEngineInitFlags initFlags) {
    // This should be set once.
    invariant(!service->getStorageEngine());

    if (0 == (initFlags & StorageEngineInitFlags::kAllowNoLockFile)) {
        createLockFile(service);
    }

    const std::string dbpath = storageGlobalParams.dbpath;

    if (!storageGlobalParams.readOnly) {
        StorageRepairObserver::set(service, std::make_unique<StorageRepairObserver>(dbpath));
        auto repairObserver = StorageRepairObserver::get(service);

        if (storageGlobalParams.repair) {
            repairObserver->onRepairStarted();
        } else if (repairObserver->isIncomplete()) {
            severe()
                << "An incomplete repair has been detected! This is likely because a repair "
                   "operation unexpectedly failed before completing. MongoDB will not start up "
                   "again without --repair.";
            fassertFailedNoTrace(50922);
        }
    }

    if (auto existingStorageEngine = StorageEngineMetadata::getStorageEngineForPath(dbpath)) {
        if (*existingStorageEngine == "mmapv1" ||
            (storageGlobalParams.engineSetByUser && storageGlobalParams.engine == "mmapv1")) {
            log() << startupWarningsLog;
            log() << "** WARNING: Support for MMAPV1 storage engine has been deprecated and will be"
                  << startupWarningsLog;
            log() << "**          removed in version 4.2. Please plan to migrate to the wiredTiger"
                  << startupWarningsLog;
            log() << "**          storage engine." << startupWarningsLog;
            log() << "**          See http://dochub.mongodb.org/core/deprecated-mmapv1";
            log() << startupWarningsLog;
        }

        if (storageGlobalParams.engineSetByUser) {
            // Verify that the name of the user-supplied storage engine matches the contents of
            // the metadata file.
            const StorageEngine::Factory* factory =
                getFactoryForStorageEngine(service, storageGlobalParams.engine);
            if (factory) {
                uassert(28662,
                        str::stream() << "Cannot start server. Detected data files in " << dbpath
                                      << " created by"
                                      << " the '"
                                      << *existingStorageEngine
                                      << "' storage engine, but the"
                                      << " specified storage engine was '"
                                      << factory->getCanonicalName()
                                      << "'.",
                        factory->getCanonicalName() == *existingStorageEngine);
            }
        } else {
            // Otherwise set the active storage engine as the contents of the metadata file.
            log() << "Detected data files in " << dbpath << " created by the '"
                  << *existingStorageEngine << "' storage engine, so setting the active"
                  << " storage engine to '" << *existingStorageEngine << "'.";
            storageGlobalParams.engine = *existingStorageEngine;
        }
    } else if (!storageGlobalParams.engineSetByUser) {
        // Ensure the default storage engine is available with this build of mongod.
        uassert(28663,
                str::stream()
                    << "Cannot start server. The default storage engine '"
                    << storageGlobalParams.engine
                    << "' is not available with this build of mongod. Please specify a different"
                    << " storage engine explicitly, e.g. --storageEngine=mmapv1.",
                isRegisteredStorageEngine(service, storageGlobalParams.engine));
    } else if (storageGlobalParams.engineSetByUser && storageGlobalParams.engine == "mmapv1") {
        log() << startupWarningsLog;
        log() << "** WARNING: You have explicitly specified 'MMAPV1' storage engine in your"
              << startupWarningsLog;
        log() << "**          config file or as a command line option.  Support for the MMAPV1"
              << startupWarningsLog;
        log() << "**          storage engine has been deprecated and will be removed in"
              << startupWarningsLog;
        log() << "**          version 4.2. See http://dochub.mongodb.org/core/deprecated-mmapv1";
        log() << startupWarningsLog;
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
                    << storageGlobalParams.engine
                    << ", does not support read-only operation",
                factory->supportsReadOnly());
    }

    std::unique_ptr<StorageEngineMetadata> metadata = StorageEngineMetadata::forPath(dbpath);

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

    auto guard = makeGuard([&] {
        auto& lockFile = StorageEngineLockFile::get(service);
        if (lockFile) {
            lockFile->close();
        }
    });

    auto& lockFile = StorageEngineLockFile::get(service);
    service->setStorageEngine(std::unique_ptr<StorageEngine>(
        factory->create(storageGlobalParams, lockFile ? &*lockFile : nullptr)));
    service->getStorageEngine()->finishInit();

    if (lockFile) {
        uassertStatusOK(lockFile->writePid());
    }

    // Write a new metadata file if it is not present.
    if (!metadata.get()) {
        invariant(!storageGlobalParams.readOnly);
        metadata.reset(new StorageEngineMetadata(storageGlobalParams.dbpath));
        metadata->setStorageEngine(factory->getCanonicalName().toString());
        metadata->setStorageEngineOptions(factory->createMetadataOptions(storageGlobalParams));
        uassertStatusOK(metadata->write());
    }

    guard.dismiss();

    _supportsDocLocking = service->getStorageEngine()->supportsDocLocking();
}

void shutdownGlobalStorageEngineCleanly(ServiceContext* service) {
    invariant(service->getStorageEngine());
    service->getStorageEngine()->cleanShutdown();
    auto& lockFile = StorageEngineLockFile::get(service);
    if (lockFile) {
        lockFile->clearPidAndUnlock();
    }
}

namespace {

void createLockFile(ServiceContext* service) {
    auto& lockFile = StorageEngineLockFile::get(service);
    try {
        lockFile.emplace(storageGlobalParams.dbpath);
    } catch (const std::exception& ex) {
        uassert(28596,
                str::stream() << "Unable to determine status of lock file in the data directory "
                              << storageGlobalParams.dbpath
                              << ": "
                              << ex.what(),
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
            severe() << "Attempted to open dbpath in readOnly mode, but the server was "
                        "previously not shut down cleanly.";
            fassertFailedNoTrace(34416);
        }
        warning() << "Detected unclean shutdown - " << lockFile->getFilespec() << " is not empty.";
        startingAfterUncleanShutdown(service) = true;
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
    stdx::function<Status(const StorageEngine::Factory* const, const BSONObj&)> validateFunc) {

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
        auto service = opCtx->getServiceContext();
        auto storageEngine = service->getStorageEngine();
        // NOTE(schwerin): The following uassert would be more desirable than the early return when
        // no storage engine is set, but to achieve that we would have to ensure that this file was
        // never linked into a test binary that didn't actually need/use the storage engine.
        //
        // uassert(<some code>,
        //         "Must instantiate storage engine before creating OperationContext",
        //         storageEngine);
        if (!storageEngine) {
            return;
        }
        opCtx->setLockState(stdx::make_unique<LockerImpl>());
        opCtx->setRecoveryUnit(std::unique_ptr<RecoveryUnit>(storageEngine->newRecoveryUnit()),
                               WriteUnitOfWork::RecoveryUnitState::kNotInUnitOfWork);
    }
    void onDestroyOperationContext(OperationContext* opCtx) {}
};

ServiceContext::ConstructorActionRegisterer registerStorageClientObserverConstructor{
    "RegisterStorageClientObserverConstructor", [](ServiceContext* service) {
        service->registerClientObserver(std::make_unique<StorageClientObserver>());
    }};

}  // namespace
}  // namespace mongo
