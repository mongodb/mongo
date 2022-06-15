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


#if defined(__linux__)
#include <sys/vfs.h>
#endif

#include "mongo/platform/basic.h"

#include "mongo/base/init.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/storage_engine_impl.h"
#include "mongo/db/storage/storage_engine_init.h"
#include "mongo/db/storage/storage_engine_lock_file.h"
#include "mongo/db/storage/storage_engine_metadata.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_global_options.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_index.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_kv_engine.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_parameters_gen.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_record_store.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_server_status.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/logv2/log.h"
#include "mongo/util/processinfo.h"

#if __has_feature(address_sanitizer)
#include <sanitizer/lsan_interface.h>
#endif

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


namespace mongo {

namespace {
std::string kWiredTigerBackupFile = "WiredTiger.backup";

class WiredTigerFactory : public StorageEngine::Factory {
public:
    virtual ~WiredTigerFactory() {}
    virtual std::unique_ptr<StorageEngine> create(OperationContext* opCtx,
                                                  const StorageGlobalParams& params,
                                                  const StorageEngineLockFile* lockFile) const {
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

        size_t cacheMB = WiredTigerUtil::getCacheSizeMB(wiredTigerGlobalOptions.cacheSizeGB);
        const double memoryThresholdPercentage = 0.8;
        ProcessInfo p;
        if (p.supported()) {
            if (cacheMB > memoryThresholdPercentage * p.getMemSizeMB()) {
                LOGV2_OPTIONS(22300,
                              {logv2::LogTag::kStartupWarnings},
                              "The configured WiredTiger cache size is more than 80% of available "
                              "RAM. See http://dochub.mongodb.org/core/faq-memory-diagnostics-wt");
            }
        }
        auto kv =
            std::make_unique<WiredTigerKVEngine>(getCanonicalName().toString(),
                                                 params.dbpath,
                                                 getGlobalServiceContext()->getFastClockSource(),
                                                 wiredTigerGlobalOptions.engineConfig,
                                                 cacheMB,
                                                 wiredTigerGlobalOptions.getMaxHistoryFileSizeMB(),
                                                 params.ephemeral,
                                                 params.repair);
        kv->setRecordStoreExtraOptions(wiredTigerGlobalOptions.collectionConfig);
        kv->setSortedDataInterfaceExtraOptions(wiredTigerGlobalOptions.indexConfig);

        // We must only add the server parameters to the global registry once during unit testing.
        static int setupCountForUnitTests = 0;
        if (setupCountForUnitTests == 0) {
            ++setupCountForUnitTests;

            // Intentionally leaked.
            [[maybe_unused]] auto leakedSection = new WiredTigerServerStatusSection();

            // This allows unit tests to run this code without encountering memory leaks
#if __has_feature(address_sanitizer)
            __lsan_ignore_object(leakedSection);
#endif
        }

        StorageEngineOptions options;
        options.directoryPerDB = params.directoryperdb;
        options.directoryForIndexes = wiredTigerGlobalOptions.directoryForIndexes;
        options.forRepair = params.repair;
        options.forRestore = params.restore;
        options.lockFileCreatedByUncleanShutdown = lockFile && lockFile->createdByUncleanShutdown();
        return std::make_unique<StorageEngineImpl>(opCtx, std::move(kv), options);
    }

    virtual StringData getCanonicalName() const {
        return kWiredTigerEngineName;
    }

    virtual Status validateCollectionStorageOptions(const BSONObj& options) const {
        return WiredTigerRecordStore::parseOptionsField(options).getStatus();
    }

    virtual Status validateIndexStorageOptions(const BSONObj& options) const {
        return WiredTigerIndex::parseIndexOptions(options).getStatus();
    }

    virtual Status validateMetadata(const StorageEngineMetadata& metadata,
                                    const StorageGlobalParams& params) const {
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

    virtual BSONObj createMetadataOptions(const StorageGlobalParams& params) const {
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
