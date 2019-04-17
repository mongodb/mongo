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

#include "mongo/db/catalog/catalog_control.h"

#include "mongo/db/catalog/database_holder_mock.h"
#include "mongo/db/client.h"
#include "mongo/db/index_builds_coordinator_mongod.h"
#include "mongo/db/operation_context_noop.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

/**
 * Mock storage engine.
 */
class MockStorageEngine : public StorageEngine {
public:
    RecoveryUnit* newRecoveryUnit() final {
        return nullptr;
    }
    std::vector<std::string> listDatabases() const final {
        return {};
    }
    bool supportsDocLocking() const final {
        return false;
    }
    bool isDurable() const final {
        return false;
    }
    bool isEphemeral() const {
        return true;
    }
    Status closeDatabase(OperationContext* opCtx, StringData db) final {
        return Status::OK();
    }
    Status dropDatabase(OperationContext* opCtx, StringData db) final {
        return Status::OK();
    }
    int flushAllFiles(OperationContext* opCtx, bool sync) final {
        return 0;
    }
    Status repairRecordStore(OperationContext* opCtx, const NamespaceString& ns) final {
        return Status::OK();
    }
    std::unique_ptr<TemporaryRecordStore> makeTemporaryRecordStore(OperationContext* opCtx) final {
        return {};
    }
    void cleanShutdown() final {}
    void setJournalListener(JournalListener* jl) final {}
    bool supportsPendingDrops() const final {
        return false;
    }
    void clearDropPendingState() final {}
    Timestamp getAllCommittedTimestamp() const final {
        return {};
    }
    Timestamp getOldestOpenReadTimestamp() const final {
        return {};
    }
    std::string getFilesystemPathForDb(const std::string& dbName) const final {
        return "";
    }
    std::set<std::string> getDropPendingIdents() const final {
        return {};
    }
    Status currentFilesCompatible(OperationContext* opCtx) const final {
        return Status::OK();
    }
    int64_t sizeOnDiskForDb(OperationContext* opCtx, StringData dbName) {
        return 0;
    }
};

/**
 * Simple test for openCatalog() and closeCatalog() to check library dependencies.
 */
class CatalogControlTest : public unittest::Test {
private:
    void setUp() override;
    void tearDown() override;

    std::unique_ptr<ThreadClient> _tc;
};

void CatalogControlTest::setUp() {
    {
        auto serviceContext = ServiceContext::make();
        auto storageEngine = std::make_unique<MockStorageEngine>();
        serviceContext->setStorageEngine(std::move(storageEngine));
        DatabaseHolder::set(serviceContext.get(), std::make_unique<DatabaseHolderMock>());
        // Only need the IndexBuildsCoordinator to call into and check whether there are any index
        // builds in progress.
        IndexBuildsCoordinator::set(serviceContext.get(),
                                    std::make_unique<IndexBuildsCoordinatorMongod>());
        setGlobalServiceContext(std::move(serviceContext));
    }

    _tc = std::make_unique<ThreadClient>(getGlobalServiceContext());
}

void CatalogControlTest::tearDown() {
    _tc = {};
}

TEST_F(CatalogControlTest, CloseAndOpenCatalog) {
    OperationContextNoop opCtx(&cc(), 0);
    auto map = catalog::closeCatalog(&opCtx);
    ASSERT_EQUALS(0U, map.size());
    catalog::openCatalog(&opCtx, {});
}

}  // namespace
}  // namespace mongo
