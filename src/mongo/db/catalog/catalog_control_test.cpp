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
    void finishInit() final {}
    RecoveryUnit* newRecoveryUnit() final {
        return nullptr;
    }
    std::vector<std::string> listDatabases() const final {
        return {};
    }
    bool supportsDocLocking() const final {
        return false;
    }
    bool supportsDBLocking() const final {
        return true;
    }
    bool supportsCappedCollections() const final {
        return true;
    }
    bool supportsCheckpoints() const final {
        return false;
    }
    bool isDurable() const final {
        return false;
    }
    bool isEphemeral() const final {
        return true;
    }
    void loadCatalog(OperationContext* opCtx) final {}
    void closeCatalog(OperationContext* opCtx) final {}
    Status closeDatabase(OperationContext* opCtx, StringData db) final {
        return Status::OK();
    }
    Status dropDatabase(OperationContext* opCtx, StringData db) final {
        return Status::OK();
    }
    int flushAllFiles(OperationContext* opCtx, bool sync) final {
        return 0;
    }
    Status beginBackup(OperationContext* opCtx) final {
        return Status(ErrorCodes::CommandNotSupported,
                      "The current storage engine doesn't support backup mode");
    }
    void endBackup(OperationContext* opCtx) final {}
    StatusWith<std::vector<StorageEngine::BackupBlock>> beginNonBlockingBackup(
        OperationContext* opCtx) final {
        return Status(ErrorCodes::CommandNotSupported,
                      "The current storage engine does not support a concurrent mode.");
    }
    void endNonBlockingBackup(OperationContext* opCtx) final {}
    StatusWith<std::vector<std::string>> extendBackupCursor(OperationContext* opCtx) final {
        return Status(ErrorCodes::CommandNotSupported,
                      "The current storage engine does not support a concurrent mode.");
    }
    Status repairRecordStore(OperationContext* opCtx, const NamespaceString& ns) final {
        return Status::OK();
    }
    std::unique_ptr<TemporaryRecordStore> makeTemporaryRecordStore(OperationContext* opCtx) final {
        return {};
    }
    void cleanShutdown() final {}
    SnapshotManager* getSnapshotManager() const final {
        return nullptr;
    }
    void setJournalListener(JournalListener* jl) final {}
    bool supportsRecoverToStableTimestamp() const final {
        return false;
    }
    bool supportsRecoveryTimestamp() const final {
        return false;
    }
    bool supportsReadConcernSnapshot() const final {
        return false;
    }
    bool supportsReadConcernMajority() const final {
        return false;
    }
    bool supportsOplogStones() const final {
        return false;
    }
    bool supportsPendingDrops() const final {
        return false;
    }
    void clearDropPendingState() final {}
    StatusWith<Timestamp> recoverToStableTimestamp(OperationContext* opCtx) final {
        fassertFailed(40547);
    }
    boost::optional<Timestamp> getRecoveryTimestamp() const final {
        MONGO_UNREACHABLE;
    }
    boost::optional<Timestamp> getLastStableRecoveryTimestamp() const final {
        MONGO_UNREACHABLE;
    }
    void setStableTimestamp(Timestamp stableTimestamp, bool force = false) final {}
    void setInitialDataTimestamp(Timestamp timestamp) final {}
    void setOldestTimestampFromStable() final {}
    void setOldestTimestamp(Timestamp timestamp) final {}
    void setOldestActiveTransactionTimestampCallback(
        OldestActiveTransactionTimestampCallback callback) final {}
    bool isCacheUnderPressure(OperationContext* opCtx) const final {
        return false;
    }
    void setCachePressureForTest(int pressure) final {}
    void replicationBatchIsComplete() const final {}
    StatusWith<std::vector<CollectionIndexNamePair>> reconcileCatalogAndIdents(
        OperationContext* opCtx) final {
        return std::vector<CollectionIndexNamePair>();
    }
    Timestamp getAllDurableTimestamp() const final {
        return {};
    }
    Timestamp getOldestOpenReadTimestamp() const final {
        return {};
    }
    boost::optional<Timestamp> getOplogNeededForCrashRecovery() const final {
        return boost::none;
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
    int64_t sizeOnDiskForDb(OperationContext* opCtx, StringData dbName) final {
        return 0;
    }
    KVEngine* getEngine() final {
        return nullptr;
    }
    const KVEngine* getEngine() const final {
        return nullptr;
    }
    DurableCatalog* getCatalog() final {
        return nullptr;
    }
    const DurableCatalog* getCatalog() const final {
        return nullptr;
    }
    std::unique_ptr<CheckpointLock> getCheckpointLock(OperationContext* opCtx) final {
        return nullptr;
    }
    void addIndividuallyCheckpointedIndexToList(const std::string& ident) final {}
    void clearIndividuallyCheckpointedIndexesList() final {}
    bool isInIndividuallyCheckpointedIndexesList(const std::string& ident) const final {
        return false;
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
