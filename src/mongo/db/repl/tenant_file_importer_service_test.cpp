/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/optional/optional.hpp>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/bsontypes_util.h"
#include "mongo/db/database_name.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/client_cursor/cursor_response.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/replica_set_aware_service.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/repl/tenant_file_importer_service.h"
#include "mongo/db/repl/tenant_migration_shard_merge_util.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/dbtests/mock/mock_dbclient_connection.h"
#include "mongo/dbtests/mock/mock_remote_db_server.h"
#include "mongo/executor/task_executor_test_fixture.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/log_severity.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/bson_test_util.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/framework.h"
#include "mongo/unittest/log_test.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/net/hostandport.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {


namespace repl {

using namespace repl::shard_merge_utils;

namespace {
constexpr auto kDonorHostName = "localhost:12345"_sd;
constexpr auto kDonorDBPath = "/path/to/remoteDB/"_sd;
static const UUID kBackupId = UUID::gen();
const OpTime kStartMigrationOpTime(Timestamp(1, 1), 1);

}  // namespace
class TenantFileImporterServiceTest : public ServiceContextMongoDTest {
public:
    /**
     * Create TenantFileImporterService::ImporterEvent::kLearnedFileName event.
     */
    static BSONObj makefileMetaDoc(const UUID& migrationId,
                                   const std::string& fileName,
                                   uint64_t fileSize) {
        return BSON("filename" << kDonorDBPath + "/" + fileName << "fileSize"
                               << static_cast<int64_t>(fileSize) << kDonorHostNameFieldName
                               << kDonorHostName << kMigrationIdFieldName << migrationId
                               << kBackupIdFieldName << kBackupId << kDonorDbPathFieldName
                               << kDonorDBPath);
    }


    /**
     * Returns true if collection exists.
     */
    static bool collectionExists(OperationContext* opCtx, const NamespaceString& nss) {
        return static_cast<bool>(AutoGetCollectionForRead(opCtx, nss).getCollection());
    }


    void setUp() override {
        ServiceContextMongoDTest::setUp();
        auto serviceContext = getServiceContext();
        auto replCoord = std::make_unique<ReplicationCoordinatorMock>(serviceContext);
        replCoord->setRunCmdOnPrimaryAndAwaitResponseFunction([this](OperationContext* opCtx,
                                                                     const DatabaseName& dbName,
                                                                     const BSONObj& cmdObj,
                                                                     ReplicationCoordinator::
                                                                         OnRemoteCmdScheduledFn
                                                                             onRemoteCmdScheduled,
                                                                     ReplicationCoordinator::
                                                                         OnRemoteCmdCompleteFn
                                                                             onRemoteCmdComplete) {
            runCmdOnPrimaryAndAwaitResponseFnCalls.push_back(RunCmdOnPrimaryCall{dbName, cmdObj});
            return runCmdOnPrimaryAndAwaitResponseFnResponse;
        });
        ASSERT_OK(replCoord->setFollowerMode(repl::MemberState::RS_PRIMARY));
        ReplicationCoordinator::set(serviceContext, std::move(replCoord));
        StorageInterface::set(serviceContext, std::make_unique<StorageInterfaceImpl>());

        _importerService = repl::TenantFileImporterService::get(serviceContext);

        _mockDonorServer = std::make_unique<MockRemoteDBServer>(kDonorHostName.toString());
        _importerService->setCreateConnectionFn_forTest([&]() {
            return std::make_unique<MockDBClientConnection>(_mockDonorServer.get(),
                                                            true /* autoReconnect */);
        });

        globalFailPointRegistry().find("skipImportFiles")->setMode(FailPoint::alwaysOn);

        // Set the stable timestamp to avoid hang in
        // TenantFileImporterService::_waitUntilStartMigrationTimestampIsCheckpointed().
        auto opCtx = cc().makeOperationContext();
        auto engine = serviceContext->getStorageEngine()->getEngine();
        engine->setStableTimestamp(Timestamp(1, 1), true);
    }

    void tearDown() override {
        _importerService->onShutdown();
        StorageInterface::set(getServiceContext(), {});
        ReplicationCoordinator::set(getServiceContext(), {});
        ServiceContextMongoDTest::tearDown();
    }

    struct RunCmdOnPrimaryCall {
        DatabaseName dbName;
        BSONObj cmdObj;
    };
    std::vector<RunCmdOnPrimaryCall> runCmdOnPrimaryAndAwaitResponseFnCalls;
    BSONObj runCmdOnPrimaryAndAwaitResponseFnResponse = BSON("ok" << 1);

private:
    unittest::MinimumLoggedSeverityGuard _replicationSeverityGuard{
        logv2::LogComponent::kReplication, logv2::LogSeverity::Debug(1)};
    unittest::MinimumLoggedSeverityGuard _tenantMigrationSeverityGuard{
        logv2::LogComponent::kTenantMigration, logv2::LogSeverity::Debug(1)};

protected:
    std::unique_ptr<MockRemoteDBServer> _mockDonorServer;
    TenantFileImporterService* _importerService;
};

TEST_F(TenantFileImporterServiceTest, ConcurrentMigrationWithDifferentMigrationID) {
    FailPointEnableBlock failPoint("skipCloneFiles");
    auto migrationId = UUID::gen();
    auto anotherMigrationId = UUID::gen();

    auto verifyAllStateTransitionFailsForAnotherMigrationId = [&] {
        ASSERT_THROWS_CODE(
            _importerService->startMigration(anotherMigrationId, kStartMigrationOpTime),
            DBException,
            7800210);
        ASSERT_THROWS_CODE(_importerService->learnedFilename(
                               anotherMigrationId, makefileMetaDoc(migrationId, "some-file.wt", 1)),
                           DBException,
                           7800210);
        ASSERT_THROWS_CODE(
            _importerService->learnedAllFilenames(anotherMigrationId), DBException, 7800210);
        ASSERT_THROWS_CODE(
            _importerService->interruptMigration(anotherMigrationId), DBException, 7800210);
        ASSERT_THROWS_CODE(
            _importerService->resetMigration(anotherMigrationId), DBException, 7800210);
    };

    _importerService->startMigration(migrationId, kStartMigrationOpTime);
    ASSERT_EQ(_importerService->getMigrationId_forTest(), migrationId);
    ASSERT_EQ(_importerService->getState_forTest(), TenantFileImporterService::State::kStarted);

    verifyAllStateTransitionFailsForAnotherMigrationId();

    _importerService->learnedFilename(migrationId, makefileMetaDoc(migrationId, "some-file.wt", 1));
    ASSERT_EQ(_importerService->getMigrationId_forTest(), migrationId);
    ASSERT_EQ(_importerService->getState_forTest(),
              TenantFileImporterService::State::kLearnedFilename);

    verifyAllStateTransitionFailsForAnotherMigrationId();

    _importerService->learnedAllFilenames(migrationId);
    ASSERT_EQ(_importerService->getMigrationId_forTest(), migrationId);
    ASSERT_EQ(_importerService->getState_forTest(),
              TenantFileImporterService::State::kLearnedAllFilenames);

    verifyAllStateTransitionFailsForAnotherMigrationId();

    _importerService->interruptMigration(migrationId);
    ASSERT_EQ(_importerService->getMigrationId_forTest(), migrationId);
    ASSERT_EQ(_importerService->getState_forTest(), TenantFileImporterService::State::kInterrupted);

    verifyAllStateTransitionFailsForAnotherMigrationId();

    _importerService->resetMigration(migrationId);
    ASSERT(!_importerService->getMigrationId_forTest());

    {
        // Starting a new migration with anotherMigrationId is now possible.
        _importerService->startMigration(anotherMigrationId, kStartMigrationOpTime);
        ASSERT_EQ(_importerService->getMigrationId_forTest(), anotherMigrationId);
        ASSERT_EQ(_importerService->getState_forTest(), TenantFileImporterService::State::kStarted);
    }
}

TEST_F(TenantFileImporterServiceTest, StartConcurrentMigrationWithSameMigrationID) {
    FailPointEnableBlock failPoint("skipCloneFiles");
    auto migrationId = UUID::gen();

    _importerService->startMigration(migrationId, kStartMigrationOpTime);
    ASSERT_EQ(_importerService->getMigrationId_forTest(), migrationId);
    ASSERT_EQ(_importerService->getState_forTest(), TenantFileImporterService::State::kStarted);

    // startMigration calls with the same migrationId will be ignored.
    _importerService->startMigration(migrationId, kStartMigrationOpTime);

    _importerService->learnedFilename(migrationId, makefileMetaDoc(migrationId, "some-file.wt", 1));
    ASSERT_EQ(_importerService->getMigrationId_forTest(), migrationId);
    ASSERT_EQ(_importerService->getState_forTest(),
              TenantFileImporterService::State::kLearnedFilename);

    ASSERT_THROWS_CODE(
        _importerService->startMigration(migrationId, kStartMigrationOpTime), DBException, 7800210);

    _importerService->learnedAllFilenames(migrationId);
    ASSERT_EQ(_importerService->getMigrationId_forTest(), migrationId);
    ASSERT_EQ(_importerService->getState_forTest(),
              TenantFileImporterService::State::kLearnedAllFilenames);

    ASSERT_THROWS_CODE(
        _importerService->startMigration(migrationId, kStartMigrationOpTime), DBException, 7800210);

    _importerService->interruptMigration(migrationId);
    ASSERT_EQ(_importerService->getMigrationId_forTest(), migrationId);
    ASSERT_EQ(_importerService->getState_forTest(), TenantFileImporterService::State::kInterrupted);

    _importerService->resetMigration(migrationId);
    ASSERT(!_importerService->getMigrationId_forTest());

    // Starting a new migration with same migrationId is now possible.
    _importerService->startMigration(migrationId, kStartMigrationOpTime);
    ASSERT_EQ(_importerService->getMigrationId_forTest(), migrationId);
    ASSERT_EQ(_importerService->getState_forTest(), TenantFileImporterService::State::kStarted);
}

TEST_F(TenantFileImporterServiceTest, ShouldHaveLearntAtLeastOneFileName) {
    auto migrationId = UUID::gen();

    _importerService->startMigration(migrationId, kStartMigrationOpTime);
    ASSERT_THROWS_CODE(_importerService->learnedAllFilenames(migrationId), DBException, 7800210);
}

TEST_F(TenantFileImporterServiceTest, learnedAllFilenamesFollowedByLearnedFileNameOutOfOrderEvent) {
    FailPointEnableBlock failPoint("skipCloneFiles");
    auto migrationId = UUID::gen();

    _importerService->startMigration(migrationId, kStartMigrationOpTime);
    _importerService->learnedFilename(migrationId, makefileMetaDoc(migrationId, "some-file.wt", 1));
    _importerService->learnedAllFilenames(migrationId);
    ASSERT_EQ(_importerService->getMigrationId_forTest(), migrationId);
    ASSERT_EQ(_importerService->getState_forTest(),
              TenantFileImporterService::State::kLearnedAllFilenames);

    ASSERT_THROWS_CODE(
        _importerService->learnedFilename(migrationId,
                                          BSON("filename"
                                               << "some-file.wt" << kDonorHostNameFieldName
                                               << kDonorHostName << "fileSize" << 1)),
        DBException,
        7800210);

    // Interrupt the migration to prevent running file cloning after exiting this block.
    _importerService->interruptMigration(migrationId);
}

TEST_F(TenantFileImporterServiceTest, MigrationNotStartedYetShouldIgnoreAnyStateTransition) {
    auto migrationId = UUID::gen();

    ASSERT(!_importerService->getMigrationId_forTest());

    _importerService->learnedFilename(migrationId, makefileMetaDoc(migrationId, "some-file.wt", 1));
    ASSERT(!_importerService->getMigrationId_forTest());

    _importerService->learnedAllFilenames(migrationId);
    ASSERT(!_importerService->getMigrationId_forTest());

    _importerService->interruptMigration(migrationId);
    ASSERT(!_importerService->getMigrationId_forTest());

    _importerService->resetMigration(migrationId);
    ASSERT(!_importerService->getMigrationId_forTest());
}

TEST_F(TenantFileImporterServiceTest, CanInterruptMigrationAfterMigrationStart) {
    auto migrationId = UUID::gen();

    _importerService->startMigration(migrationId, kStartMigrationOpTime);
    ASSERT_EQ(_importerService->getMigrationId_forTest(), migrationId);
    ASSERT_EQ(_importerService->getState_forTest(), TenantFileImporterService::State::kStarted);

    _importerService->interruptMigration(migrationId);
    ASSERT_EQ(_importerService->getMigrationId_forTest(), migrationId);
    ASSERT_EQ(_importerService->getState_forTest(), TenantFileImporterService::State::kInterrupted);
}

TEST_F(TenantFileImporterServiceTest, CanInterruptMigrationWhenLearnedFileName) {
    FailPointEnableBlock failPoint("skipCloneFiles");
    auto migrationId = UUID::gen();

    _importerService->startMigration(migrationId, kStartMigrationOpTime);
    _importerService->learnedFilename(migrationId, makefileMetaDoc(migrationId, "some-file.wt", 1));
    ASSERT_EQ(_importerService->getMigrationId_forTest(), migrationId);
    ASSERT_EQ(_importerService->getState_forTest(),
              TenantFileImporterService::State::kLearnedFilename);

    _importerService->interruptMigration(migrationId);
    ASSERT_EQ(_importerService->getMigrationId_forTest(), migrationId);
    ASSERT_EQ(_importerService->getState_forTest(), TenantFileImporterService::State::kInterrupted);
}

TEST_F(TenantFileImporterServiceTest, CanInterruptMigrationWhenLearnedAllFileNames) {
    FailPointEnableBlock failPoint("skipCloneFiles");
    auto migrationId = UUID::gen();

    _importerService->startMigration(migrationId, kStartMigrationOpTime);
    _importerService->learnedFilename(migrationId, makefileMetaDoc(migrationId, "some-file.wt", 1));
    _importerService->learnedAllFilenames(migrationId);
    ASSERT_EQ(_importerService->getMigrationId_forTest(), migrationId);
    ASSERT_EQ(_importerService->getState_forTest(),
              TenantFileImporterService::State::kLearnedAllFilenames);

    _importerService->interruptMigration(migrationId);
    ASSERT_EQ(_importerService->getMigrationId_forTest(), migrationId);
    ASSERT_EQ(_importerService->getState_forTest(), TenantFileImporterService::State::kInterrupted);
}

TEST_F(TenantFileImporterServiceTest, CanInterruptAMigrationMoreThanOnce) {
    auto migrationId = UUID::gen();
    _importerService->startMigration(migrationId, kStartMigrationOpTime);
    _importerService->interruptMigration(migrationId);
    ASSERT_EQ(_importerService->getMigrationId_forTest(), migrationId);
    ASSERT_EQ(_importerService->getState_forTest(), TenantFileImporterService::State::kInterrupted);

    _importerService->interruptMigration(migrationId);
}

TEST_F(TenantFileImporterServiceTest, InterruptedMigrationCannotLearnNewFiles) {
    auto migrationId = UUID::gen();

    _importerService->startMigration(migrationId, kStartMigrationOpTime);
    _importerService->interruptMigration(migrationId);
    ASSERT_EQ(_importerService->getMigrationId_forTest(), migrationId);
    ASSERT_EQ(_importerService->getState_forTest(), TenantFileImporterService::State::kInterrupted);

    ASSERT_THROWS_CODE(_importerService->learnedFilename(migrationId,
                                                         BSON("filename"
                                                              << "some-file.wt"
                                                              << "fileSize" << 1)),
                       DBException,
                       7800210);
    ASSERT_THROWS_CODE(_importerService->learnedAllFilenames(migrationId), DBException, 7800210);
}

TEST_F(TenantFileImporterServiceTest, resetMigration) {
    FailPointEnableBlock failPoint("skipCloneFiles");
    auto migrationId = UUID::gen();

    _importerService->startMigration(migrationId, kStartMigrationOpTime);
    ASSERT_EQ(_importerService->getMigrationId_forTest(), migrationId);
    ASSERT_EQ(_importerService->getState_forTest(), TenantFileImporterService::State::kStarted);

    ASSERT_THROWS_CODE(_importerService->resetMigration(migrationId), DBException, 7800210);

    _importerService->learnedFilename(migrationId, makefileMetaDoc(migrationId, "some-file.wt", 1));
    ASSERT_EQ(_importerService->getMigrationId_forTest(), migrationId);
    ASSERT_EQ(_importerService->getState_forTest(),
              TenantFileImporterService::State::kLearnedFilename);

    ASSERT_THROWS_CODE(_importerService->resetMigration(migrationId), DBException, 7800210);

    _importerService->learnedAllFilenames(migrationId);
    ASSERT_EQ(_importerService->getMigrationId_forTest(), migrationId);
    ASSERT_EQ(_importerService->getState_forTest(),
              TenantFileImporterService::State::kLearnedAllFilenames);

    ASSERT_THROWS_CODE(_importerService->resetMigration(migrationId), DBException, 7800210);

    _importerService->interruptMigration(migrationId);
    ASSERT_EQ(_importerService->getMigrationId_forTest(), migrationId);
    ASSERT_EQ(_importerService->getState_forTest(), TenantFileImporterService::State::kInterrupted);

    _importerService->resetMigration(migrationId);
    ASSERT(!_importerService->getMigrationId_forTest());

    // Resetting migration again shouldn't throw.
    _importerService->resetMigration(migrationId);
}

TEST_F(TenantFileImporterServiceTest, ImportsFilesWhenAllFilenamesLearned) {
    FailPointEnableBlock hangBeforeFileImporterThreadExit("hangBeforeFileImporterThreadExit");

    auto fpSkipImportFiles = globalFailPointRegistry().find("skipImportFiles");
    const auto fpSkipImportFilesInitialTimesEntered =
        fpSkipImportFiles->toBSON()["timesEntered"].safeNumberLong();
    auto migrationId = UUID::gen();

    const std::string fileName = "some-file.wt";
    std::string fileData = "Here is the file data";
    CursorResponse fileAggResponse(
        NamespaceString::makeCollectionlessAggregateNSS(DatabaseName::kAdmin),
        0 /* cursorId */,
        {BSON("byteOffset" << 0 << "endOfFile" << true << "data"
                           << BSONBinData(fileData.data(), fileData.size(), BinDataGeneral))});

    _mockDonorServer->setCommandReply("aggregate", fileAggResponse.toBSONAsInitialResponse());

    // Verify that the temp WT db path is empty before migration start.
    auto tempWTDirectory = fileClonerTempDir(migrationId);
    ASSERT(!boost::filesystem::exists(tempWTDirectory / fileName));

    _importerService->startMigration(migrationId, kStartMigrationOpTime);
    _importerService->learnedFilename(migrationId,
                                      makefileMetaDoc(migrationId, fileName, fileData.size()));
    _importerService->learnedAllFilenames(migrationId);
    ASSERT_EQ(_importerService->getMigrationId_forTest(), migrationId);
    ASSERT_EQ(_importerService->getState_forTest(),
              TenantFileImporterService::State::kLearnedAllFilenames);

    hangBeforeFileImporterThreadExit->waitForTimesEntered(
        hangBeforeFileImporterThreadExit.initialTimesEntered() + 1);

    // Verify that the files have been cloned successfully.
    ASSERT(boost::filesystem::exists(tempWTDirectory / fileName));
    ASSERT_EQ(fileData.size(), boost::filesystem::file_size(tempWTDirectory / fileName));

    // Verify if the import files operation has been called.
    fpSkipImportFiles->waitForTimesEntered(fpSkipImportFilesInitialTimesEntered + 1);

    // Check if the import done marker collection exists.
    ASSERT(collectionExists(makeOperationContext().get(), getImportDoneMarkerNs(migrationId)));

    // Verify whether the node has notified the primary about the import success.
    ASSERT_EQ(runCmdOnPrimaryAndAwaitResponseFnCalls.size(), 1);
    auto recipientVoteImportedFilesCmdCall = runCmdOnPrimaryAndAwaitResponseFnCalls.front();
    ASSERT_EQ(recipientVoteImportedFilesCmdCall.dbName, DatabaseName::kAdmin);
    ASSERT_BSONOBJ_EQ(recipientVoteImportedFilesCmdCall.cmdObj,
                      BSON("recipientVoteImportedFiles" << 1 << "migrationId" << migrationId
                                                        << "from"
                                                        << ":27017"));
}

TEST_F(TenantFileImporterServiceTest, statsForInvalidMigrationID) {
    auto migrationId = UUID::gen();
    auto invalidMigrationID = UUID::gen();

    _importerService->startMigration(migrationId, kStartMigrationOpTime);
    ASSERT_EQ(_importerService->getMigrationId_forTest(), migrationId);
    ASSERT_EQ(_importerService->getState_forTest(), TenantFileImporterService::State::kStarted);

    auto stats = _importerService->getStats(invalidMigrationID);
    ASSERT_TRUE(stats.isEmpty());
}

TEST_F(TenantFileImporterServiceTest, statsForValidMigrationID) {
    auto migrationId = UUID::gen();

    const std::string file1Name = "some-file1.wt";
    std::string file1Data = "Here is the file1 data";
    CursorResponse file1AggResponse(
        NamespaceString::makeCollectionlessAggregateNSS(DatabaseName::kAdmin),
        0 /* cursorId */,
        {BSON("byteOffset" << 0 << "endOfFile" << true << "data"
                           << BSONBinData(file1Data.data(), file1Data.size(), BinDataGeneral))});

    const std::string file2Name = "some-file2.wt";
    std::string file2Data = "Here is the file2 data";
    CursorResponse file2AggResponse(
        NamespaceString::makeCollectionlessAggregateNSS(DatabaseName::kAdmin),
        0 /* cursorId */,
        {BSON("byteOffset" << 0 << "endOfFile" << true << "data"
                           << BSONBinData(file2Data.data(), file2Data.size(), BinDataGeneral))});

    _mockDonorServer->setCommandReply(
        "aggregate",
        {file1AggResponse.toBSONAsInitialResponse(), file2AggResponse.toBSONAsInitialResponse()});

    const auto totalDataSize = file1Data.size() + file2Data.size();
    FailPointEnableBlock hangBeforeFileImporterThreadExit("hangBeforeFileImporterThreadExit");

    // Verify that the stat is empty before migration start.
    auto stats = _importerService->getStats(migrationId);
    ASSERT(stats.isEmpty());

    _importerService->startMigration(migrationId, kStartMigrationOpTime);
    // Sleep to prevent the race with "totalReceiveElapsedMillis" field.
    mongo::sleepmillis(1);

    stats = _importerService->getStats(migrationId);
    ASSERT(!stats.isEmpty());
    ASSERT(stats.hasField("approxTotalDataSize"));
    ASSERT(stats.hasField("approxTotalBytesCopied"));
    ASSERT(stats.hasField("totalReceiveElapsedMillis"));
    ASSERT(stats.hasField("remainingReceiveEstimatedMillis"));
    ASSERT_EQ(stats["approxTotalDataSize"].safeNumberLong(), 0ll);
    ASSERT_EQ(stats["approxTotalBytesCopied"].safeNumberLong(), 0ll);
    ASSERT_GT(stats["totalReceiveElapsedMillis"].safeNumberLong(), 0ll);
    ASSERT_EQ(stats["remainingReceiveEstimatedMillis"].safeNumberLong(), 0ll);

    {
        FailPointEnableBlock fpTenantFileClonerHangDuringFileCloneBackup(
            "TenantFileClonerHangDuringFileCloneBackup");

        _importerService->learnedFilename(
            migrationId, makefileMetaDoc(migrationId, file1Name, file1Data.size()));
        _importerService->learnedFilename(
            migrationId, makefileMetaDoc(migrationId, file2Name, file2Data.size()));

        fpTenantFileClonerHangDuringFileCloneBackup->waitForTimesEntered(
            fpTenantFileClonerHangDuringFileCloneBackup.initialTimesEntered() + 1);
        stats = _importerService->getStats(migrationId);
        ASSERT(!stats.isEmpty());
        ASSERT(stats.hasField("approxTotalDataSize"));
        ASSERT(stats.hasField("approxTotalBytesCopied"));
        ASSERT(stats.hasField("totalReceiveElapsedMillis"));
        ASSERT(stats.hasField("remainingReceiveEstimatedMillis"));
        ASSERT_EQ(stats["approxTotalDataSize"].safeNumberLong(), totalDataSize);
        ASSERT_EQ(stats["approxTotalBytesCopied"].safeNumberLong(), file1Data.size());
        ASSERT_GT(stats["totalReceiveElapsedMillis"].safeNumberLong(), 0ll);
        ASSERT_GT(stats["remainingReceiveEstimatedMillis"].safeNumberLong(), 0ll);
    }


    _importerService->learnedAllFilenames(migrationId);

    hangBeforeFileImporterThreadExit->waitForTimesEntered(
        hangBeforeFileImporterThreadExit.initialTimesEntered() + 1);
    stats = _importerService->getStats(migrationId);
    ASSERT(!stats.isEmpty());
    ASSERT(stats.hasField("approxTotalDataSize"));
    ASSERT(stats.hasField("approxTotalBytesCopied"));
    ASSERT(stats.hasField("totalReceiveElapsedMillis"));
    ASSERT(stats.hasField("remainingReceiveEstimatedMillis"));
    ASSERT_EQ(stats["approxTotalDataSize"].safeNumberLong(), totalDataSize);
    ASSERT_EQ(stats["approxTotalBytesCopied"].safeNumberLong(), totalDataSize);
    ASSERT_GT(stats["totalReceiveElapsedMillis"].safeNumberLong(), 0ll);
    ASSERT_EQ(stats["remainingReceiveEstimatedMillis"].safeNumberLong(), 0ll);
}

}  // namespace repl
}  // namespace mongo
