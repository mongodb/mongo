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


#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
// IWYU pragma: no_include "cxxabi.h"
#include <memory>
#include <set>
#include <string>
#include <system_error>
#include <variant>
#include <vector>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/commands.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/db/ops/write_ops_gen.h"
#include "mongo/db/ops/write_ops_parsers.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/query_request_helper.h"
#include "mongo/db/record_id.h"
#include "mongo/db/storage/duplicate_key_error_info.h"
#include "mongo/db/write_concern.h"
#include "mongo/executor/network_connection_hook.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/network_test_env.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/rpc/write_concern_error_detail.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog/sharding_catalog_client_impl.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/sharding_mongos_test_fixture.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/bson_test_util.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/uuid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


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

using InsertRetryTest = ShardingTestFixture;
using UpdateRetryTest = ShardingTestFixture;

const NamespaceString kTestNamespace =
    NamespaceString::createNamespaceString_forTest("config.TestColl");
const HostAndPort kTestHosts[] = {
    HostAndPort("TestHost1:12345"), HostAndPort("TestHost2:12345"), HostAndPort("TestHost3:12345")};

Status getMockDuplicateKeyError() {
    return {DuplicateKeyErrorInfo(
                BSON("mock" << 1), BSON("" << 1), BSONObj{}, std::monostate{}, boost::none),
            "Mock duplicate key error"};
}

TEST_F(InsertRetryTest, RetryOnInterruptedAndNetworkErrorSuccess) {
    configTargeter()->setFindHostReturnValue({kTestHosts[0]});

    BSONObj objToInsert = BSON("_id" << 1 << "Value"
                                     << "TestValue");

    auto future = launchAsync([&] {
        Status status =
            catalogClient()->insertConfigDocument(operationContext(),
                                                  kTestNamespace,
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

    future.default_timed_get();
}

TEST_F(InsertRetryTest, RetryOnNetworkErrorFails) {
    configTargeter()->setFindHostReturnValue({kTestHosts[0]});

    BSONObj objToInsert = BSON("_id" << 1 << "Value"
                                     << "TestValue");

    auto future = launchAsync([&] {
        Status status =
            catalogClient()->insertConfigDocument(operationContext(),
                                                  kTestNamespace,
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

    future.default_timed_get();
}

void assertFindRequestHasFilter(const RemoteCommandRequest& request, BSONObj filter) {
    // If there is no '$db', append it.
    auto cmd = static_cast<OpMsgRequest>(request).body;
    auto query = query_request_helper::makeFromFindCommandForTests(cmd);
    ASSERT_BSONOBJ_EQ(filter, query->getFilter());
}

TEST_F(InsertRetryTest, DuplicateKeyErrorAfterNetworkErrorMatch) {
    configTargeter()->setFindHostReturnValue({kTestHosts[0]});

    BSONObj objToInsert = BSON("_id" << 1 << "Value"
                                     << "TestValue");

    auto future = launchAsync([&] {
        Status status =
            catalogClient()->insertConfigDocument(operationContext(),
                                                  kTestNamespace,
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
        return getMockDuplicateKeyError();
    });

    onFindCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(request.target, kTestHosts[1]);
        assertFindRequestHasFilter(request, BSON("_id" << 1));

        return vector<BSONObj>{objToInsert};
    });

    future.default_timed_get();
}

TEST_F(InsertRetryTest, DuplicateKeyErrorAfterNetworkErrorNotFound) {
    configTargeter()->setFindHostReturnValue({kTestHosts[0]});

    BSONObj objToInsert = BSON("_id" << 1 << "Value"
                                     << "TestValue");

    auto future = launchAsync([&] {
        Status status =
            catalogClient()->insertConfigDocument(operationContext(),
                                                  kTestNamespace,
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
        return getMockDuplicateKeyError();
    });

    onFindCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(request.target, kTestHosts[1]);
        assertFindRequestHasFilter(request, BSON("_id" << 1));
        return vector<BSONObj>();
    });

    future.default_timed_get();
}

TEST_F(InsertRetryTest, DuplicateKeyErrorAfterNetworkErrorMismatch) {
    configTargeter()->setFindHostReturnValue({kTestHosts[0]});

    BSONObj objToInsert = BSON("_id" << 1 << "Value"
                                     << "TestValue");

    auto future = launchAsync([&] {
        Status status =
            catalogClient()->insertConfigDocument(operationContext(),
                                                  kTestNamespace,
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
        return getMockDuplicateKeyError();
    });

    onFindCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(request.target, kTestHosts[1]);
        assertFindRequestHasFilter(request, BSON("_id" << 1));

        return vector<BSONObj>{BSON("_id" << 1 << "Value"
                                          << "TestValue has changed")};
    });

    future.default_timed_get();
}

TEST_F(InsertRetryTest, DuplicateKeyErrorAfterWriteConcernFailureMatch) {
    configTargeter()->setFindHostReturnValue({kTestHosts[0]});

    BSONObj objToInsert = BSON("_id" << 1 << "Value"
                                     << "TestValue");

    auto future = launchAsync([&] {
        Status status =
            catalogClient()->insertConfigDocument(operationContext(),
                                                  kTestNamespace,
                                                  objToInsert,
                                                  ShardingCatalogClient::kMajorityWriteConcern);
        ASSERT_OK(status);
    });

    onCommand([&](const RemoteCommandRequest& request) {
        const auto opMsgRequest = static_cast<OpMsgRequest>(request);
        const auto insertOp = InsertOp::parse(opMsgRequest);
        ASSERT_EQUALS(kTestNamespace, insertOp.getNamespace());

        BatchedCommandResponse response;
        response.setStatus(Status::OK());
        response.setN(1);

        auto wcError = std::make_unique<WriteConcernErrorDetail>();

        WriteConcernResult wcRes;
        wcRes.err = "timeout";
        wcRes.wTimedOut = true;

        wcError->setStatus({ErrorCodes::NetworkTimeout, "Failed to wait for write concern"});
        wcError->setErrInfo(BSON("wtimeout" << true));

        response.setWriteConcernError(wcError.release());

        return response.toBSON();
    });

    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(request.target, kTestHosts[0]);
        return getMockDuplicateKeyError();
    });

    onFindCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(request.target, kTestHosts[0]);
        assertFindRequestHasFilter(request, BSON("_id" << 1));

        return vector<BSONObj>{objToInsert};
    });

    future.default_timed_get();
}

TEST_F(UpdateRetryTest, Success) {
    configTargeter()->setFindHostReturnValue(HostAndPort("TestHost1"));

    BSONObj objToUpdate = BSON("_id" << 1 << "Value"
                                     << "TestValue");
    BSONObj updateExpr = BSON("$set" << BSON("Value"
                                             << "NewTestValue"));

    auto future = launchAsync([&] {
        auto status =
            catalogClient()->updateConfigDocument(operationContext(),
                                                  kTestNamespace,
                                                  objToUpdate,
                                                  updateExpr,
                                                  false,
                                                  ShardingCatalogClient::kMajorityWriteConcern);
        ASSERT_OK(status);
    });

    onCommand([&](const RemoteCommandRequest& request) {
        const auto opMsgRequest = static_cast<OpMsgRequest>(request);
        const auto updateOp = UpdateOp::parse(opMsgRequest);
        ASSERT_EQUALS(kTestNamespace, updateOp.getNamespace());

        BatchedCommandResponse response;
        response.setStatus(Status::OK());
        response.setNModified(1);

        return response.toBSON();
    });

    future.default_timed_get();
}

TEST_F(UpdateRetryTest, NotWritablePrimaryErrorReturnedPersistently) {
    configTargeter()->setFindHostReturnValue(HostAndPort("TestHost1"));

    BSONObj objToUpdate = BSON("_id" << 1 << "Value"
                                     << "TestValue");
    BSONObj updateExpr = BSON("$set" << BSON("Value"
                                             << "NewTestValue"));

    auto future = launchAsync([&] {
        auto status =
            catalogClient()->updateConfigDocument(operationContext(),
                                                  kTestNamespace,
                                                  objToUpdate,
                                                  updateExpr,
                                                  false,
                                                  ShardingCatalogClient::kMajorityWriteConcern);
        ASSERT_EQUALS(ErrorCodes::NotWritablePrimary, status);
    });

    for (int i = 0; i < 3; ++i) {
        onCommand([](const RemoteCommandRequest& request) {
            BSONObjBuilder bb;
            CommandHelpers::appendCommandStatusNoThrow(
                bb, {ErrorCodes::NotWritablePrimary, "not primary"});
            return bb.obj();
        });
    }

    future.default_timed_get();
}

TEST_F(UpdateRetryTest, NotWritablePrimaryReturnedFromTargeter) {
    configTargeter()->setFindHostReturnValue(Status(ErrorCodes::NotWritablePrimary, "not primary"));

    BSONObj objToUpdate = BSON("_id" << 1 << "Value"
                                     << "TestValue");
    BSONObj updateExpr = BSON("$set" << BSON("Value"
                                             << "NewTestValue"));

    auto future = launchAsync([&] {
        auto status =
            catalogClient()->updateConfigDocument(operationContext(),
                                                  kTestNamespace,
                                                  objToUpdate,
                                                  updateExpr,
                                                  false,
                                                  ShardingCatalogClient::kMajorityWriteConcern);
        ASSERT_EQUALS(ErrorCodes::NotWritablePrimary, status);
    });

    future.default_timed_get();
}

TEST_F(UpdateRetryTest, NotWritablePrimaryOnceSuccessAfterRetry) {
    HostAndPort host1("TestHost1");
    HostAndPort host2("TestHost2");
    configTargeter()->setFindHostReturnValue(host1);

    CollectionType collection(NamespaceString::createNamespaceString_forTest("db.coll"),
                              OID::gen(),
                              Timestamp(1, 1),
                              network()->now(),
                              UUID::gen(),
                              BSON("_id" << 1));

    BSONObj objToUpdate = BSON("_id" << 1 << "Value"
                                     << "TestValue");
    BSONObj updateExpr = BSON("$set" << BSON("Value"
                                             << "NewTestValue"));

    auto future = launchAsync([&] {
        ASSERT_OK(
            catalogClient()->updateConfigDocument(operationContext(),
                                                  kTestNamespace,
                                                  objToUpdate,
                                                  updateExpr,
                                                  false,
                                                  ShardingCatalogClient::kMajorityWriteConcern));
    });

    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQUALS(host1, request.target);

        // Ensure that when the catalog manager tries to retarget after getting the
        // NotWritablePrimary response, it will get back a new target.
        configTargeter()->setFindHostReturnValue(host2);

        BSONObjBuilder bb;
        CommandHelpers::appendCommandStatusNoThrow(bb,
                                                   {ErrorCodes::NotWritablePrimary, "not primary"});
        return bb.obj();
    });

    onCommand([&](const RemoteCommandRequest& request) {
        const auto opMsgRequest = static_cast<OpMsgRequest>(request);
        const auto updateOp = UpdateOp::parse(opMsgRequest);
        ASSERT_EQUALS(kTestNamespace, updateOp.getNamespace());

        BatchedCommandResponse response;
        response.setStatus(Status::OK());
        response.setNModified(1);

        return response.toBSON();
    });

    future.default_timed_get();
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
                                                  kTestNamespace,
                                                  objToUpdate,
                                                  updateExpr,
                                                  false,
                                                  ShardingCatalogClient::kMajorityWriteConcern);
        ASSERT_OK(status);
    });

    onCommand([&](const RemoteCommandRequest& request) {
        const auto opMsgRequest = static_cast<OpMsgRequest>(request);
        const auto updateOp = UpdateOp::parse(opMsgRequest);
        ASSERT_EQUALS(kTestNamespace, updateOp.getNamespace());

        BatchedCommandResponse response;
        response.setStatus(Status::OK());
        response.addToErrDetails(write_ops::WriteError(
            0, {ErrorCodes::InterruptedDueToReplStateChange, "Operation interrupted"}));

        return response.toBSON();
    });

    onCommand([&](const RemoteCommandRequest& request) {
        const auto opMsgRequest = static_cast<OpMsgRequest>(request);
        const auto updateOp = UpdateOp::parse(opMsgRequest);
        ASSERT_EQUALS(kTestNamespace, updateOp.getNamespace());

        BatchedCommandResponse response;
        response.setStatus(Status::OK());
        response.setNModified(1);

        return response.toBSON();
    });

    future.default_timed_get();
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
                                                  kTestNamespace,
                                                  objToUpdate,
                                                  updateExpr,
                                                  false,
                                                  ShardingCatalogClient::kMajorityWriteConcern);
        ASSERT_OK(status);
    });

    onCommand([&](const RemoteCommandRequest& request) {
        const auto opMsgRequest = static_cast<OpMsgRequest>(request);
        const auto updateOp = UpdateOp::parse(opMsgRequest);
        ASSERT_EQUALS(kTestNamespace, updateOp.getNamespace());

        BatchedCommandResponse response;
        response.setStatus(Status::OK());
        response.setNModified(1);

        auto wcError = std::make_unique<WriteConcernErrorDetail>();

        WriteConcernResult wcRes;
        wcRes.err = "timeout";
        wcRes.wTimedOut = true;

        wcError->setStatus({ErrorCodes::NetworkTimeout, "Failed to wait for write concern"});
        wcError->setErrInfo(BSON("wtimeout" << true));

        response.setWriteConcernError(wcError.release());

        return response.toBSON();
    });

    onCommand([&](const RemoteCommandRequest& request) {
        const auto opMsgRequest = static_cast<OpMsgRequest>(request);
        const auto updateOp = UpdateOp::parse(opMsgRequest);
        ASSERT_EQUALS(kTestNamespace, updateOp.getNamespace());

        BatchedCommandResponse response;
        response.setStatus(Status::OK());
        response.setNModified(0);

        return response.toBSON();
    });

    future.default_timed_get();
}

}  // namespace
}  // namespace mongo
