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

#include "mongo/platform/basic.h"

#include "mongo/base/init.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/ephemeral_for_test/ephemeral_for_test_kv_engine.h"
#include "mongo/db/storage/ephemeral_for_test/ephemeral_for_test_record_store.h"
#include "mongo/db/storage/ephemeral_for_test/ephemeral_for_test_server_status.h"
#include "mongo/db/storage/storage_engine_impl.h"
#include "mongo/db/storage/storage_engine_init.h"
#include "mongo/db/storage/storage_engine_lock_file.h"
#include "mongo/db/storage/storage_options.h"

#if __has_feature(address_sanitizer)
#include <sanitizer/lsan_interface.h>
#endif

namespace mongo {
namespace ephemeral_for_test {

namespace {
class EphemeralForTestStorageEngineFactory : public StorageEngine::Factory {
public:
    virtual std::unique_ptr<StorageEngine> create(OperationContext* opCtx,
                                                  const StorageGlobalParams& params,
                                                  const StorageEngineLockFile* lockFile) const {
        auto kv = std::make_unique<KVEngine>();
        // We must only add the server parameters to the global registry once during unit testing.
        static int setupCountForUnitTests = 0;
        if (setupCountForUnitTests == 0) {
            ++setupCountForUnitTests;

            // Intentionally leaked.
            [[maybe_unused]] auto leakedSection = new ServerStatusSection(kv.get());

            // This allows unit tests to run this code without encountering memory leaks
#if __has_feature(address_sanitizer)
            __lsan_ignore_object(leakedSection);
#endif
        }

        StorageEngineOptions options;
        options.directoryPerDB = params.directoryperdb;
        options.forRepair = params.repair;
        options.lockFileCreatedByUncleanShutdown = lockFile && lockFile->createdByUncleanShutdown();
        return std::make_unique<StorageEngineImpl>(opCtx, std::move(kv), options);
    }

    virtual StringData getCanonicalName() const {
        return kEngineName;
    }

    virtual Status validateMetadata(const StorageEngineMetadata& metadata,
                                    const StorageGlobalParams& params) const {
        return Status::OK();
    }

    virtual BSONObj createMetadataOptions(const StorageGlobalParams& params) const {
        return BSONObj();
    }
};


ServiceContext::ConstructorActionRegisterer registerEphemeralForTest(
    "RegisterEphemeralForTestEngine", [](ServiceContext* service) {
        registerStorageEngine(service, std::make_unique<EphemeralForTestStorageEngineFactory>());
    });

}  // namespace
}  // namespace ephemeral_for_test
}  // namespace mongo
