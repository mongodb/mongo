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
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/replica_set_aware_service.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/repl/tenant_file_importer_service.h"
#include "mongo/db/repl/tenant_migration_shard_merge_util.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/dbtests/mock/mock_dbclient_connection.h"
#include "mongo/dbtests/mock/mock_remote_db_server.h"
#include "mongo/executor/task_executor_test_fixture.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/log_severity.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/bson_test_util.h"
#include "mongo/unittest/framework.h"
#include "mongo/unittest/log_test.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/net/hostandport.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {

using executor::TaskExecutor;
using executor::ThreadPoolExecutorTest;

namespace repl {
class TenantFileImporterServiceTest : public ServiceContextMongoDTest {
public:
    void setUp() override {
        ServiceContextMongoDTest::setUp();
        auto serviceContext = getServiceContext();
        auto replCoord = std::make_unique<ReplicationCoordinatorMock>(serviceContext);
        replCoord->setRunCmdOnPrimaryAndAwaitResponseFunction([this](OperationContext* opCtx,
                                                                     const std::string& dbName,
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
    }

    void tearDown() override {
        ReplicaSetAwareServiceRegistry::get(getServiceContext()).onShutdown();
        StorageInterface::set(getServiceContext(), {});
        ReplicationCoordinator::set(getServiceContext(), {});
        ServiceContextMongoDTest::tearDown();
    }

    struct RunCmdOnPrimaryCall {
        std::string dbName;
        BSONObj cmdObj;
    };
    std::vector<RunCmdOnPrimaryCall> runCmdOnPrimaryAndAwaitResponseFnCalls;
    BSONObj runCmdOnPrimaryAndAwaitResponseFnResponse = BSON("ok" << 1);

private:
    unittest::MinimumLoggedSeverityGuard _replicationSeverityGuard{
        logv2::LogComponent::kReplication, logv2::LogSeverity::Debug(1)};
    unittest::MinimumLoggedSeverityGuard _tenantMigrationSeverityGuard{
        logv2::LogComponent::kTenantMigration, logv2::LogSeverity::Debug(1)};
};

TEST_F(TenantFileImporterServiceTest, WillNotStartConcurrentMigrationsForDifferentMigrationIds) {
    auto tenantFileImporterService = repl::TenantFileImporterService::get(getServiceContext());

    auto migrationId = UUID::gen();
    tenantFileImporterService->startMigration(migrationId);

    // startMigration calls for other migrationIds are ignored.
    tenantFileImporterService->startMigration(UUID::gen());

    auto state = tenantFileImporterService->getState();
    ASSERT_EQ(state["migrationId"].str(), migrationId.toString());
    ASSERT_EQ(state["state"].str(), "started");
}

TEST_F(TenantFileImporterServiceTest, WillNotStartConcurrentMigrationsForTheSameMigrationId) {
    auto tenantFileImporterService = repl::TenantFileImporterService::get(getServiceContext());

    auto migrationId = UUID::gen();
    tenantFileImporterService->startMigration(migrationId);

    // startMigration calls with the same migrationId are ignored.
    tenantFileImporterService->startMigration(migrationId);

    auto state = tenantFileImporterService->getState();
    ASSERT_EQ(state["migrationId"].str(), migrationId.toString());
    ASSERT_EQ(state["state"].str(), "started");
}

TEST_F(TenantFileImporterServiceTest, CanBeSafelyInterruptedBeforeMigrationStart) {
    auto tenantFileImporterService = repl::TenantFileImporterService::get(getServiceContext());
    tenantFileImporterService->interruptAll();
    auto state = tenantFileImporterService->getState();
    ASSERT_EQ(state["state"].str(), "uninitialized");
}

TEST_F(TenantFileImporterServiceTest, CanBeSafelyInterruptedAfterMigrationStart) {
    auto tenantFileImporterService = repl::TenantFileImporterService::get(getServiceContext());

    auto migrationId = UUID::gen();
    tenantFileImporterService->startMigration(migrationId);
    auto state = tenantFileImporterService->getState();
    ASSERT_EQ(state["migrationId"].str(), migrationId.toString());
    ASSERT_EQ(state["state"].str(), "started");

    tenantFileImporterService->interruptAll();
    state = tenantFileImporterService->getState();
    ASSERT_EQ(state["migrationId"].str(), migrationId.toString());
    ASSERT_EQ(state["state"].str(), "interrupted");
}

TEST_F(TenantFileImporterServiceTest, ImportsFilesWhenAllFilenamesLearned) {
    auto migrationId = UUID::gen();
    auto tenantFileImporterService = repl::TenantFileImporterService::get(getServiceContext());

    std::string fileData = "Here is the file data";
    auto bindata = BSONBinData(fileData.data(), fileData.size(), BinDataGeneral);
    CursorResponse aggResponse(
        NamespaceString::makeCollectionlessAggregateNSS(DatabaseName::kAdmin),
        0 /* cursorId */,
        {BSON("byteOffset" << 0 << "endOfFile" << true << "data" << bindata)});

    MockRemoteDBServer server("test");
    auto conn = std::make_shared<MockDBClientConnection>(&server);
    tenantFileImporterService->setCreateConnectionForTest([&]() { return conn; });

    int importFilesCallCount = 0;
    tenantFileImporterService->setImportFilesForTest(
        [&](OperationContext* opCtx, const UUID& migrationId) { importFilesCallCount++; });

    server.setCommandReply("aggregate", aggResponse.toBSONAsInitialResponse());

    auto filePath = shard_merge_utils::fileClonerTempDir(migrationId);
    auto metadataDoc =
        BSON("filename" << filePath.string() + "/some-file.wt" << shard_merge_utils::kDonorFieldName
                        << server.getServerHostAndPort().toString()
                        << shard_merge_utils::kMigrationIdFieldName << migrationId
                        << shard_merge_utils::kBackupIdFieldName << UUID::gen() << "remoteDbPath"
                        << filePath.string() << "fileSize" << std::to_string(fileData.size())
                        << shard_merge_utils::kDonorDbPathFieldName << filePath.string());

    auto hangBeforeFileImporterThreadExit =
        globalFailPointRegistry().find("hangBeforeFileImporterThreadExit");
    hangBeforeFileImporterThreadExit->setMode(FailPoint::alwaysOn);

    tenantFileImporterService->startMigration(migrationId);

    tenantFileImporterService->learnedFilename(migrationId, metadataDoc);
    auto state = tenantFileImporterService->getState();
    ASSERT_EQ(state["migrationId"].str(), migrationId.toString());
    ASSERT_EQ(state["state"].str(), "learned filename");

    tenantFileImporterService->learnedAllFilenames(migrationId);

    state = tenantFileImporterService->getState();
    ASSERT_EQ(state["migrationId"].str(), migrationId.toString());
    ASSERT_EQ(state["state"].str(), "learned all filenames");

    hangBeforeFileImporterThreadExit->waitForTimesEntered(1);

    ASSERT(boost::filesystem::exists(filePath.string() + "/some-file.wt"));
    ASSERT_EQ(fileData.size(), boost::filesystem::file_size(filePath.string() + "/some-file.wt"));
    ASSERT_EQ(importFilesCallCount, 1);
    ASSERT_EQ(runCmdOnPrimaryAndAwaitResponseFnCalls.size(), 1);

    auto recipientVoteImportedFilesCmdCall = runCmdOnPrimaryAndAwaitResponseFnCalls.front();
    ASSERT_EQ(recipientVoteImportedFilesCmdCall.dbName, DatabaseName::kAdmin.db().toString());
    ASSERT_BSONOBJ_EQ(recipientVoteImportedFilesCmdCall.cmdObj,
                      BSON("recipientVoteImportedFiles" << 1 << "migrationId" << migrationId
                                                        << "from"
                                                        << ":27017"
                                                        << "success" << true));

    hangBeforeFileImporterThreadExit->setMode(FailPoint::off);
}
}  // namespace repl
}  // namespace mongo
