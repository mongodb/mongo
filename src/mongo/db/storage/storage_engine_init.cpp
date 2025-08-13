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

#include "mongo/db/storage/storage_engine_init.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/scoped_timer.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/control/storage_control.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/recovery_unit_noop.h"
#include "mongo/db/storage/storage_engine_change_context.h"
#include "mongo/db/storage/storage_engine_lock_file.h"
#include "mongo/db/storage/storage_engine_metadata.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/db/storage/storage_repair_observer.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"

#include <exception>
#include <map>
#include <string>
#include <utility>

#include <boost/filesystem/path.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo {
namespace {
/**
 * Creates the lock file used to prevent concurrent processes from accessing the data files,
 * as appropriate.
 */
void createLockFile(ServiceContext* service);
}  // namespace

StorageEngine::LastShutdownState initializeStorageEngine(
    OperationContext* opCtx,
    const StorageEngineInitFlags initFlags,
    bool isReplSet,
    bool shouldRecoverFromOplogAsStandalone,
    bool inStandaloneMode,
    BSONObjBuilder* startupTimeElapsedBuilder) {
    ServiceContext* service = opCtx->getServiceContext();

    // This should be set once.
    if ((initFlags & StorageEngineInitFlags::kForRestart) == StorageEngineInitFlags{})
        invariant(!service->getStorageEngine());

    if ((initFlags & StorageEngineInitFlags::kAllowNoLockFile) == StorageEngineInitFlags{}) {
        SectionScopedTimer scopedTimer(service->getFastClockSource(),
                                       TimedSectionId::createLockFile,
                                       startupTimeElapsedBuilder);
        createLockFile(service);
    }

    const std::string dbpath = storageGlobalParams.dbpath;

    StorageRepairObserver::set(service, std::make_unique<StorageRepairObserver>(dbpath));
    auto repairObserver = StorageRepairObserver::get(service);

    if (storageGlobalParams.repair) {
        repairObserver->onRepairStarted();
    } else if (repairObserver->isIncomplete()) {
        LOGV2_FATAL_NOTRACE(50922,
                            "An incomplete repair has been detected! This is likely because a "
                            "repair operation unexpectedly failed before completing. MongoDB will "
                            "not start up again without --repair.");
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

    if (storageGlobalParams.queryableBackupMode) {
        uassert(34368,
                str::stream() << "Server was started in queryable backup mode, but the configured "
                              << "storage engine, " << storageGlobalParams.engine
                              << ", does not support queryable backup mode",
                factory->supportsQueryableBackupMode());
    }

    std::unique_ptr<StorageEngineMetadata> metadata;
    if ((initFlags & StorageEngineInitFlags::kSkipMetadataFile) == StorageEngineInitFlags{}) {
        SectionScopedTimer scopedTimer(service->getFastClockSource(),
                                       TimedSectionId::getStorageEngineMetadata,
                                       startupTimeElapsedBuilder);
        metadata = StorageEngineMetadata::forPath(dbpath);
    }

    // Validate options in metadata against current startup options.
    if (metadata.get()) {
        SectionScopedTimer scopedTimer(service->getFastClockSource(),
                                       TimedSectionId::validateMetadata,
                                       startupTimeElapsedBuilder);
        uassertStatusOK(factory->validateMetadata(*metadata, storageGlobalParams));
    }

    // This should be set once during startup.
    if ((initFlags & StorageEngineInitFlags::kForRestart) == StorageEngineInitFlags{}) {
    }

    ScopeGuard guard([&] {
        auto& lockFile = StorageEngineLockFile::get(service);
        if (lockFile) {
            lockFile->close();
        }
    });

    auto& lockFile = StorageEngineLockFile::get(service);
    {
        SectionScopedTimer scopedTimer(service->getFastClockSource(),
                                       TimedSectionId::createStorageEngine,
                                       startupTimeElapsedBuilder);
        if ((initFlags & StorageEngineInitFlags::kForRestart) == StorageEngineInitFlags{}) {
            auto storageEngine =
                std::unique_ptr<StorageEngine>(factory->create(opCtx,
                                                               storageGlobalParams,
                                                               lockFile ? &*lockFile : nullptr,
                                                               isReplSet,
                                                               shouldRecoverFromOplogAsStandalone,
                                                               inStandaloneMode));
            service->setStorageEngine(std::move(storageEngine));
        } else {
            auto storageEngineChangeContext = StorageEngineChangeContext::get(service);
            auto lk = storageEngineChangeContext->killOpsForStorageEngineChange(service);
            auto storageEngine =
                std::unique_ptr<StorageEngine>(factory->create(opCtx,
                                                               storageGlobalParams,
                                                               lockFile ? &*lockFile : nullptr,
                                                               isReplSet,
                                                               shouldRecoverFromOplogAsStandalone,
                                                               inStandaloneMode));
            storageEngineChangeContext->changeStorageEngine(
                service, std::move(lk), std::move(storageEngine));
        }
    }

    if (lockFile) {
        SectionScopedTimer scopedTimer(
            service->getFastClockSource(), TimedSectionId::writePID, startupTimeElapsedBuilder);
        uassertStatusOK(lockFile->writePid());
    }

    // Write a new metadata file if it is not present.
    if (!metadata.get() &&
        (initFlags & StorageEngineInitFlags::kSkipMetadataFile) == StorageEngineInitFlags{}) {
        SectionScopedTimer scopedTimer(service->getFastClockSource(),
                                       TimedSectionId::writeNewMetadata,
                                       startupTimeElapsedBuilder);
        metadata.reset(new StorageEngineMetadata(storageGlobalParams.dbpath));
        metadata->setStorageEngine(std::string{factory->getCanonicalName()});
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

void shutdownGlobalStorageEngineCleanly(ServiceContext* service, bool memLeakAllowed) {
    auto storageEngine = service->getStorageEngine();
    invariant(storageEngine);
    storageEngine->cleanShutdown(service, memLeakAllowed);
    auto& lockFile = StorageEngineLockFile::get(service);
    if (lockFile) {
        lockFile->clearPidAndUnlock();
        lockFile = boost::none;
    }
}

StorageEngine::LastShutdownState reinitializeStorageEngine(
    OperationContext* opCtx,
    StorageEngineInitFlags initFlags,
    bool isReplSet,
    bool shouldRecoverFromOplogAsStandalone,
    bool inStandaloneMode,
    std::function<void()> changeConfigurationCallback) {
    auto service = opCtx->getServiceContext();
    shard_role_details::getRecoveryUnit(opCtx)->abandonSnapshot();
    // Tell storage engine to free memory since the process is not exiting.
    shutdownGlobalStorageEngineCleanly(service, false /* memLeakAllowed */);
    shard_role_details::setRecoveryUnit(opCtx,
                                        std::make_unique<RecoveryUnitNoop>(),
                                        WriteUnitOfWork::RecoveryUnitState::kNotInUnitOfWork);
    changeConfigurationCallback();
    auto lastShutdownState =
        initializeStorageEngine(opCtx,
                                initFlags | StorageEngineInitFlags::kForRestart,
                                isReplSet,
                                shouldRecoverFromOplogAsStandalone,
                                inStandaloneMode);
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
    if (openStatus == ErrorCodes::IllegalOperation) {
        lockFile = boost::none;
    } else {
        uassertStatusOK(openStatus);
    }

    if (wasUnclean) {
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

    auto name = std::string{factory->getCanonicalName()};
    storageFactories(service).emplace(name, std::move(factory));
}

bool isRegisteredStorageEngine(ServiceContext* service, StringData name) {
    return getFactoryForStorageEngine(service, name);
}

StorageEngine::Factory* getFactoryForStorageEngine(ServiceContext* service, StringData name) {
    const auto result = storageFactories(service).find(std::string{name});
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
        if (storageElement.type() != BSONType::object) {
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

std::vector<StringData> getStorageEngineNames(ServiceContext* svcCtx) {
    const auto& factories = storageFactories(svcCtx);
    std::vector<StringData> ret;
    std::transform(factories.begin(), factories.end(), std::back_inserter(ret), [](auto& it) {
        return StringData(it.first);
    });
    return ret;
}

}  // namespace mongo
