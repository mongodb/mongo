/**
 *    Copyright (C) 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include <set>
#include <string>
#include <vector>

#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/write_concern.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/task_executor.h"
#include "mongo/rpc/metadata/repl_set_metadata.h"
#include "mongo/rpc/metadata/server_selection_metadata.h"
#include "mongo/s/catalog/dist_lock_manager_mock.h"
#include "mongo/s/catalog/replset/sharding_catalog_client_impl.h"
#include "mongo/s/catalog/replset/sharding_catalog_test_fixture.h"
#include "mongo/s/catalog/type_changelog.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_database.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/stdx/future.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

using executor::NetworkInterfaceMock;
using executor::RemoteCommandRequest;
using executor::RemoteCommandResponse;
using executor::TaskExecutor;
using std::set;
using std::string;
using std::vector;
using unittest::assertGet;

using InsertRetryTest = ShardingCatalogTestFixture;
using UpdateRetryTest = ShardingCatalogTestFixture;

const NamespaceString kTestNamespace("config.TestColl");
const HostAndPort kTestHosts[] = {
    HostAndPort("TestHost1:12345"), HostAndPort("TestHost2:12345"), HostAndPort("TestHost3:12345")};

TEST_F(InsertRetryTest, RetryOnInterruptedAndNetworkErrorSuccess) {
    configTargeter()->setFindHostReturnValue({kTestHosts[0]});

    BSONObj objToInsert = BSON("_id" << 1 << "Value"
                                     << "TestValue");

    auto future = launchAsync([&] {
        Status status =
            catalogClient()->insertConfigDocument(operationContext(),
                                                  kTestNamespace.ns(),
                                                  objToInsert,
                                                  ShardingCatalogClient::kMajorityWriteConcern);
        ASSERT_OK(status);
    });

    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(request.target, kTestHosts[0]);
        configTargeter()->setFindHostReturnValue({kTestHosts[1]});
        return Status(ErrorCodes::InterruptedDueToReplStateChange, "Interruption");
    });

    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(request.target, kTestHosts[1]);
        configTargeter()->setFindHostReturnValue({kTestHosts[2]});
        return Status(ErrorCodes::NetworkTimeout, "Network timeout");
    });

    expectInserts(kTestNamespace, {objToInsert});

    future.timed_get(kFutureTimeout);
}

TEST_F(InsertRetryTest, RetryOnNetworkErrorFails) {
    configTargeter()->setFindHostReturnValue({kTestHosts[0]});

    BSONObj objToInsert = BSON("_id" << 1 << "Value"
                                     << "TestValue");

    auto future = launchAsync([&] {
        Status status =
            catalogClient()->insertConfigDocument(operationContext(),
                                                  kTestNamespace.ns(),
                                                  objToInsert,
                                                  ShardingCatalogClient::kMajorityWriteConcern);
        ASSERT_EQ(ErrorCodes::NetworkTimeout, status.code());
    });

    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(request.target, kTestHosts[0]);
        configTargeter()->setFindHostReturnValue({kTestHosts[1]});
        return Status(ErrorCodes::NetworkTimeout, "Network timeout");
    });

    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(request.target, kTestHosts[1]);
        configTargeter()->setFindHostReturnValue({kTestHosts[2]});
        return Status(ErrorCodes::NetworkTimeout, "Network timeout");
    });

    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(request.target, kTestHosts[2]);
        return Status(ErrorCodes::NetworkTimeout, "Network timeout");
    });

    future.timed_get(kFutureTimeout);
}

TEST_F(InsertRetryTest, DuplicateKeyErrorAfterNetworkErrorMatch) {
    configTargeter()->setFindHostReturnValue({kTestHosts[0]});

    BSONObj objToInsert = BSON("_id" << 1 << "Value"
                                     << "TestValue");

    auto future = launchAsync([&] {
        Status status =
            catalogClient()->insertConfigDocument(operationContext(),
                                                  kTestNamespace.ns(),
                                                  objToInsert,
                                                  ShardingCatalogClient::kMajorityWriteConcern);
        ASSERT_OK(status);
    });

    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(request.target, kTestHosts[0]);
        configTargeter()->setFindHostReturnValue({kTestHosts[1]});
        return Status(ErrorCodes::NetworkTimeout, "Network timeout");
    });

    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(request.target, kTestHosts[1]);
        return Status(ErrorCodes::DuplicateKey, "Duplicate key");
    });

    onFindCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(request.target, kTestHosts[1]);
        auto query =
            assertGet(QueryRequest::makeFromFindCommand(kTestNamespace, request.cmdObj, false));
        ASSERT_EQ(BSON("_id" << 1), query->getFilter());

        return vector<BSONObj>{objToInsert};
    });

    future.timed_get(kFutureTimeout);
}

TEST_F(InsertRetryTest, DuplicateKeyErrorAfterNetworkErrorNotFound) {
    configTargeter()->setFindHostReturnValue({kTestHosts[0]});

    BSONObj objToInsert = BSON("_id" << 1 << "Value"
                                     << "TestValue");

    auto future = launchAsync([&] {
        Status status =
            catalogClient()->insertConfigDocument(operationContext(),
                                                  kTestNamespace.ns(),
                                                  objToInsert,
                                                  ShardingCatalogClient::kMajorityWriteConcern);
        ASSERT_EQ(ErrorCodes::DuplicateKey, status.code());
    });

    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(request.target, kTestHosts[0]);
        configTargeter()->setFindHostReturnValue({kTestHosts[1]});
        return Status(ErrorCodes::NetworkTimeout, "Network timeout");
    });

    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(request.target, kTestHosts[1]);
        return Status(ErrorCodes::DuplicateKey, "Duplicate key");
    });

    onFindCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(request.target, kTestHosts[1]);
        auto query =
            assertGet(QueryRequest::makeFromFindCommand(kTestNamespace, request.cmdObj, false));
        ASSERT_EQ(BSON("_id" << 1), query->getFilter());

        return vector<BSONObj>();
    });

    future.timed_get(kFutureTimeout);
}

TEST_F(InsertRetryTest, DuplicateKeyErrorAfterNetworkErrorMismatch) {
    configTargeter()->setFindHostReturnValue({kTestHosts[0]});

    BSONObj objToInsert = BSON("_id" << 1 << "Value"
                                     << "TestValue");

    auto future = launchAsync([&] {
        Status status =
            catalogClient()->insertConfigDocument(operationContext(),
                                                  kTestNamespace.ns(),
                                                  objToInsert,
                                                  ShardingCatalogClient::kMajorityWriteConcern);
        ASSERT_EQ(ErrorCodes::DuplicateKey, status.code());
    });

    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(request.target, kTestHosts[0]);
        configTargeter()->setFindHostReturnValue({kTestHosts[1]});
        return Status(ErrorCodes::NetworkTimeout, "Network timeout");
    });

    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(request.target, kTestHosts[1]);
        return Status(ErrorCodes::DuplicateKey, "Duplicate key");
    });

    onFindCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(request.target, kTestHosts[1]);
        auto query =
            assertGet(QueryRequest::makeFromFindCommand(kTestNamespace, request.cmdObj, false));
        ASSERT_EQ(BSON("_id" << 1), query->getFilter());

        return vector<BSONObj>{BSON("_id" << 1 << "Value"
                                          << "TestValue has changed")};
    });

    future.timed_get(kFutureTimeout);
}

TEST_F(InsertRetryTest, DuplicateKeyErrorAfterWriteConcernFailureMatch) {
    configTargeter()->setFindHostReturnValue({kTestHosts[0]});

    BSONObj objToInsert = BSON("_id" << 1 << "Value"
                                     << "TestValue");

    auto future = launchAsync([&] {
        Status status =
            catalogClient()->insertConfigDocument(operationContext(),
                                                  kTestNamespace.ns(),
                                                  objToInsert,
                                                  ShardingCatalogClient::kMajorityWriteConcern);
        ASSERT_OK(status);
    });

    onCommand([&](const RemoteCommandRequest& request) {
        BatchedInsertRequest actualBatchedInsert;
        std::string errmsg;
        ASSERT_TRUE(actualBatchedInsert.parseBSON(request.dbname, request.cmdObj, &errmsg));
        ASSERT_EQUALS(kTestNamespace.ns(), actualBatchedInsert.getNS().ns());

        BatchedCommandResponse response;
        response.setOk(true);
        response.setN(1);

        auto wcError = stdx::make_unique<WriteConcernErrorDetail>();

        WriteConcernResult wcRes;
        wcRes.err = "timeout";
        wcRes.wTimedOut = true;

        Status wcStatus(ErrorCodes::NetworkTimeout, "Failed to wait for write concern");
        wcError->setErrCode(wcStatus.code());
        wcError->setErrMessage(wcStatus.reason());
        wcError->setErrInfo(BSON("wtimeout" << true));

        response.setWriteConcernError(wcError.release());

        return response.toBSON();
    });

    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(request.target, kTestHosts[0]);
        return Status(ErrorCodes::DuplicateKey, "Duplicate key");
    });

    onFindCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(request.target, kTestHosts[0]);
        auto query =
            assertGet(QueryRequest::makeFromFindCommand(kTestNamespace, request.cmdObj, false));
        ASSERT_EQ(BSON("_id" << 1), query->getFilter());

        return vector<BSONObj>{objToInsert};
    });

    future.timed_get(kFutureTimeout);
}

TEST_F(UpdateRetryTest, OperationInterruptedDueToPrimaryStepDown) {
    configTargeter()->setFindHostReturnValue({kTestHosts[0]});

    BSONObj objToUpdate = BSON("_id" << 1 << "Value"
                                     << "TestValue");
    BSONObj updateExpr = BSON("$set" << BSON("Value"
                                             << "NewTestValue"));

    auto future = launchAsync([&] {
        auto status =
            catalogClient()->updateConfigDocument(operationContext(),
                                                  kTestNamespace.ns(),
                                                  objToUpdate,
                                                  updateExpr,
                                                  false,
                                                  ShardingCatalogClient::kMajorityWriteConcern);
        ASSERT_OK(status);
    });

    onCommand([&](const RemoteCommandRequest& request) {
        BatchedUpdateRequest actualBatchedUpdate;
        std::string errmsg;
        ASSERT_TRUE(actualBatchedUpdate.parseBSON(request.dbname, request.cmdObj, &errmsg));
        ASSERT_EQUALS(kTestNamespace.ns(), actualBatchedUpdate.getNS().ns());

        BatchedCommandResponse response;

        auto writeErrDetail = stdx::make_unique<WriteErrorDetail>();
        writeErrDetail->setIndex(0);
        writeErrDetail->setErrCode(ErrorCodes::InterruptedDueToReplStateChange);
        writeErrDetail->setErrMessage("Operation interrupted");
        response.addToErrDetails(writeErrDetail.release());

        return response.toBSON();
    });

    onCommand([&](const RemoteCommandRequest& request) {
        BatchedUpdateRequest actualBatchedUpdate;
        std::string errmsg;
        ASSERT_TRUE(actualBatchedUpdate.parseBSON(request.dbname, request.cmdObj, &errmsg));
        ASSERT_EQUALS(kTestNamespace.ns(), actualBatchedUpdate.getNS().ns());

        BatchedCommandResponse response;
        response.setOk(true);
        response.setNModified(1);

        return response.toBSON();
    });

    future.timed_get(kFutureTimeout);
}

TEST_F(UpdateRetryTest, WriteConcernFailure) {
    configTargeter()->setFindHostReturnValue({kTestHosts[0]});

    BSONObj objToUpdate = BSON("_id" << 1 << "Value"
                                     << "TestValue");
    BSONObj updateExpr = BSON("$set" << BSON("Value"
                                             << "NewTestValue"));

    auto future = launchAsync([&] {
        auto status =
            catalogClient()->updateConfigDocument(operationContext(),
                                                  kTestNamespace.ns(),
                                                  objToUpdate,
                                                  updateExpr,
                                                  false,
                                                  ShardingCatalogClient::kMajorityWriteConcern);
        ASSERT_OK(status);
    });

    onCommand([&](const RemoteCommandRequest& request) {
        BatchedUpdateRequest actualBatchedUpdate;
        std::string errmsg;
        ASSERT_TRUE(actualBatchedUpdate.parseBSON(request.dbname, request.cmdObj, &errmsg));
        ASSERT_EQUALS(kTestNamespace.ns(), actualBatchedUpdate.getNS().ns());

        BatchedCommandResponse response;
        response.setOk(true);
        response.setNModified(1);

        auto wcError = stdx::make_unique<WriteConcernErrorDetail>();

        WriteConcernResult wcRes;
        wcRes.err = "timeout";
        wcRes.wTimedOut = true;

        Status wcStatus(ErrorCodes::NetworkTimeout, "Failed to wait for write concern");
        wcError->setErrCode(wcStatus.code());
        wcError->setErrMessage(wcStatus.reason());
        wcError->setErrInfo(BSON("wtimeout" << true));

        response.setWriteConcernError(wcError.release());

        return response.toBSON();
    });

    onCommand([&](const RemoteCommandRequest& request) {
        BatchedUpdateRequest actualBatchedUpdate;
        std::string errmsg;
        ASSERT_TRUE(actualBatchedUpdate.parseBSON(request.dbname, request.cmdObj, &errmsg));
        ASSERT_EQUALS(kTestNamespace.ns(), actualBatchedUpdate.getNS().ns());

        BatchedCommandResponse response;
        response.setOk(true);
        response.setNModified(0);

        return response.toBSON();
    });

    future.timed_get(kFutureTimeout);
}

}  // namespace
}  // namespace mongo
