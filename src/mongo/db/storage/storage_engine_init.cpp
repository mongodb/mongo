// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/storage/storage_engine_init.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/scoped_timer.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/storage/control/storage_control.h"
#include "mongo/db/storage/oplog_cap_maintainer_thread.h"
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
#include <string_view>
#include <utility>

#include <boost/filesystem/path.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo {
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
}

StorageEngine::LastShutdownState reinitializeStorageEngine(
    OperationContext* opCtx,
    StorageEngineInitFlags initFlags,
    bool isReplSet,
    bool shouldRecoverFromOplogAsStandalone,
    bool shouldSkipOplogSampling,
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
    startOplogCapMaintainerThread(service, isReplSet, shouldSkipOplogSampling);
    return lastShutdownState;
}

namespace {

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

bool isRegisteredStorageEngine(ServiceContext* service, std::string_view name) {
    return getFactoryForStorageEngine(service, name);
}

StorageEngine::Factory* getFactoryForStorageEngine(ServiceContext* service, std::string_view name) {
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
        std::string_view storageEngineName = storageElement.fieldNameStringData();
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

std::vector<std::string_view> getStorageEngineNames(ServiceContext* svcCtx) {
    const auto& factories = storageFactories(svcCtx);
    std::vector<std::string_view> ret;
    std::transform(factories.begin(), factories.end(), std::back_inserter(ret), [](auto& it) {
        return std::string_view(it.first);
    });
    return ret;
}

}  // namespace mongo
