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

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/rss/replicated_storage_service.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/storage_engine_impl.h"
#include "mongo/db/storage/storage_engine_init.h"
#include "mongo/db/storage/storage_engine_lock_file.h"
#include "mongo/db/storage/storage_engine_metadata.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/db/storage/wiredtiger/spill_wiredtiger_kv_engine.h"
#include "mongo/db/storage/wiredtiger/spill_wiredtiger_server_status.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_extensions.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_global_options.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_global_options_gen.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_index.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_kv_engine.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_record_store.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_server_status.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/logv2/log.h"
#include "mongo/util/processinfo.h"

#include <cstddef>
#include <memory>
#include <string>
#include <utility>

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/optional/optional.hpp>

#if defined(__linux__)
#include <sys/statfs.h>  // IWYU pragma: keep
#include <sys/vfs.h>     // IWYU pragma: keep
#endif

#if __has_feature(address_sanitizer)
#include <sanitizer/lsan_interface.h>
#endif

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


namespace mongo {

namespace {
std::string kWiredTigerBackupFile = "WiredTiger.backup";
std::once_flag wiredTigerServerStatusSectionFlag;
std::once_flag spillWiredTigerServerStatusSectionFlag;

class WiredTigerFactory : public StorageEngine::Factory {
public:
    ~WiredTigerFactory() override {}

    std::unique_ptr<StorageEngine> create(OperationContext* opCtx,
                                          const StorageGlobalParams& params,
                                          const StorageEngineLockFile* lockFile,
                                          bool isReplSet,
                                          bool shouldRecoverFromOplogAsStandalone,
                                          bool inStandaloneMode) const override {
        if (lockFile && lockFile->createdByUncleanShutdown()) {
            LOGV2_WARNING(22302, "Recovering data from the last clean checkpoint.");

            // If we had an unclean shutdown during an ongoing backup remove WiredTiger.backup. This
            // allows WT to use checkpoints taken while the backup cursor was open for recovery.
            boost::filesystem::path basePath(storageGlobalParams.dbpath);
            if (boost::filesystem::remove(basePath / WiredTigerBackup::kOngoingBackupFile)) {
                if (boost::filesystem::remove(basePath / kWiredTigerBackupFile)) {
                    LOGV2_INFO(
                        5844600,
                        "Removing WiredTiger.backup to allow recovery from any checkpoints taken "
                        "during ongoing backup.");
                } else {
                    LOGV2_INFO(5844601, "WiredTiger.backup doesn't exist, cleanup not needed.");
                }
            }
        }

#if defined(__linux__)
// This is from <linux/magic.h> but that isn't available on all systems.
// Note that the magic number for ext4 is the same as ext2 and ext3.
#define EXT4_SUPER_MAGIC 0xEF53
        {
            struct statfs fs_stats;
            int ret = statfs(params.dbpath.c_str(), &fs_stats);

            if (ret == 0 && fs_stats.f_type == EXT4_SUPER_MAGIC) {
                LOGV2_OPTIONS(22297,
                              {logv2::LogTag::kStartupWarnings},
                              "Using the XFS filesystem is strongly recommended with the "
                              "WiredTiger storage engine. See "
                              "http://dochub.mongodb.org/core/prodnotes-filesystem");
            }
        }
#endif

        size_t cacheMB = WiredTigerUtil::getMainCacheSizeMB(wiredTigerGlobalOptions.cacheSizeGB,
                                                            wiredTigerGlobalOptions.cacheSizePct);
        ProcessInfo p;
        if (p.supported()) {
            if (cacheMB > WiredTigerUtil::memoryThresholdPercentage * p.getMemSizeMB()) {
                LOGV2_OPTIONS(22300,
                              {logv2::LogTag::kStartupWarnings},
                              "The configured WiredTiger cache size is more than 80% of available "
                              "RAM. See http://dochub.mongodb.org/core/faq-memory-diagnostics-wt");
            }
        }

        auto& provider = rss::ReplicatedStorageService::get(opCtx).getPersistenceProvider();
        WiredTigerKVEngineBase::WiredTigerConfig wtConfig =
            getWiredTigerConfigFromStartupOptions(provider);
        wtConfig.cacheSizeMB = cacheMB;
        wtConfig.inMemory = params.inMemory;
        if (params.inMemory) {
            wtConfig.logEnabled = false;
        }
        wtConfig.zstdCompressorLevel = wiredTigerGlobalOptions.zstdCompressorLevel;
        auto kv = std::make_unique<WiredTigerKVEngine>(
            std::string{getCanonicalName()},
            params.dbpath,
            &opCtx->fastClockSource(),
            std::move(wtConfig),
            WiredTigerExtensions::get(opCtx->getServiceContext()),
            provider,
            params.repair,
            isReplSet,
            shouldRecoverFromOplogAsStandalone,
            inStandaloneMode);
        kv->setRecordStoreExtraOptions(wiredTigerGlobalOptions.collectionConfig);
        kv->setSortedDataInterfaceExtraOptions(wiredTigerGlobalOptions.indexConfig);

        std::unique_ptr<SpillWiredTigerKVEngine> spillWiredTigerKVEngine;
        if (feature_flags::gFeatureFlagCreateSpillKVEngine.isEnabled()) {
            boost::system::error_code ec;
            boost::filesystem::remove_all(params.getSpillDbPath(), ec);
            if (ec) {
                LOGV2_WARNING(10380300,
                              "Failed to clear dbpath of the internal WiredTiger instance",
                              "error"_attr = ec.message());
            }

            WiredTigerKVEngineBase::WiredTigerConfig wtConfig =
                getSpillWiredTigerConfigFromStartupOptions();
            spillWiredTigerKVEngine = std::make_unique<SpillWiredTigerKVEngine>(
                std::string{getCanonicalName()},
                params.getSpillDbPath(),
                &opCtx->fastClockSource(),
                std::move(wtConfig),
                SpillWiredTigerExtensions::get(opCtx->getServiceContext()));

            std::call_once(spillWiredTigerServerStatusSectionFlag, [] {
                *ServerStatusSectionBuilder<SpillWiredTigerServerStatusSection>(
                     std::string{SpillWiredTigerServerStatusSection::kServerStatusSectionName})
                     .forShard();
            });
        }

        // We're using the WT engine; register the ServerStatusSection for it.
        // Only do so once; even if we re-create the StorageEngine for FCBIS. The section is
        // stateless.
        std::call_once(wiredTigerServerStatusSectionFlag, [] {
            *ServerStatusSectionBuilder<WiredTigerServerStatusSection>(
                 std::string{WiredTigerServerStatusSection::kServerStatusSectionName})
                 .forShard();
        });

        StorageEngineOptions options;
        options.directoryPerDB = params.directoryperdb;
        options.directoryForIndexes = wiredTigerGlobalOptions.directoryForIndexes;
        options.forRepair = params.repair;
        options.forRestore = params.restore;
        options.lockFileCreatedByUncleanShutdown = lockFile && lockFile->createdByUncleanShutdown();
        return std::make_unique<StorageEngineImpl>(
            opCtx, std::move(kv), std::move(spillWiredTigerKVEngine), options);
    }

    StringData getCanonicalName() const override {
        return kWiredTigerEngineName;
    }

    Status validateCollectionStorageOptions(const BSONObj& options) const override {
        return WiredTigerRecordStore::parseOptionsField(options).getStatus();
    }

    Status validateIndexStorageOptions(const BSONObj& options) const override {
        return WiredTigerIndex::parseIndexOptions(options).getStatus();
    }

    Status validateMetadata(const StorageEngineMetadata& metadata,
                            const StorageGlobalParams& params) const override {
        Status status =
            metadata.validateStorageEngineOption("directoryPerDB", params.directoryperdb);
        if (!status.isOK()) {
            return status;
        }

        status = metadata.validateStorageEngineOption("directoryForIndexes",
                                                      wiredTigerGlobalOptions.directoryForIndexes);
        if (!status.isOK()) {
            return status;
        }

        // If the 'groupCollections' field does not exist in the 'storage.bson' file, the
        // data-format of existing tables is as if 'groupCollections' is false. Passing this in
        // prevents validation from accepting 'params.groupCollections' being true when a "group
        // collections" aware mongod is launched on an 3.4- dbpath.
        const bool kDefaultGroupCollections = false;
        status =
            metadata.validateStorageEngineOption("groupCollections",
                                                 params.groupCollections,
                                                 boost::optional<bool>(kDefaultGroupCollections));
        if (!status.isOK()) {
            return status;
        }

        return Status::OK();
    }

    BSONObj createMetadataOptions(const StorageGlobalParams& params) const override {
        BSONObjBuilder builder;
        builder.appendBool("directoryPerDB", params.directoryperdb);
        builder.appendBool("directoryForIndexes", wiredTigerGlobalOptions.directoryForIndexes);
        builder.appendBool("groupCollections", params.groupCollections);
        return builder.obj();
    }

    bool supportsQueryableBackupMode() const final {
        return true;
    }
};

ServiceContext::ConstructorActionRegisterer registerWiredTiger(
    "WiredTigerEngineInit", [](ServiceContext* service) {
        registerStorageEngine(service, std::make_unique<WiredTigerFactory>());
    });

}  // namespace
}  // namespace mongo
