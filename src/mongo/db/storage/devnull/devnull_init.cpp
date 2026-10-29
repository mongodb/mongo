// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/devnull/devnull_kv_engine.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/storage_engine_impl.h"
#include "mongo/db/storage/storage_engine_init.h"
#include "mongo/db/storage/storage_engine_lock_file.h"
#include "mongo/db/storage/storage_options.h"

#include <memory>
#include <string>
#include <string_view>

namespace mongo {

namespace {
class DevNullStorageEngineFactory : public StorageEngine::Factory {
public:
    std::unique_ptr<StorageEngine> create(OperationContext* opCtx,
                                          const StorageGlobalParams& params,
                                          const StorageEngineLockFile* lockFile,
                                          bool isReplSet,
                                          bool shouldRecoverFromOplogAsStandalone,
                                          bool inStandaloneMode) const override {
        StorageEngineOptions options;
        options.directoryPerDB = params.directoryperdb;
        options.forRepair = params.repair;
        options.forRestore = params.restore;
        options.lockFileCreatedByUncleanShutdown = lockFile && lockFile->createdByUncleanShutdown();
        return std::make_unique<StorageEngineImpl>(
            opCtx, std::make_unique<DevNullKVEngine>(), std::unique_ptr<KVEngine>(), options);
    }

    std::string_view getCanonicalName() const override {
        return "devnull";
    }

    Status validateMetadata(const StorageEngineMetadata& metadata,
                            const StorageGlobalParams& params) const override {
        return Status::OK();
    }

    BSONObj createMetadataOptions(const StorageGlobalParams& params) const override {
        return BSONObj();
    }
};
}  // namespace

ServiceContext::ConstructorActionRegisterer registerDevNull(
    "RegisterDevNullEngine", [](ServiceContext* service) {
        registerStorageEngine(service, std::make_unique<DevNullStorageEngineFactory>());
    });
}  // namespace mongo
