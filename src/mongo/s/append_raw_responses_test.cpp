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

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/client/connection_string.h"
#include "mongo/db/commands.h"
#include "mongo/db/global_catalog/sharding_catalog_client.h"
#include "mongo/db/global_catalog/sharding_catalog_client_mock.h"
#include "mongo/db/global_catalog/type_shard.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/optime_with.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/router_role/cluster_commands_helpers.h"
#include "mongo/db/sharding_environment/shard_handle.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/sharding_environment/sharding_mongos_test_fixture.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/rpc/write_concern_error_detail.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/str.h"
#include "mongo/util/uuid.h"

#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo {
namespace {

const HostAndPort kTestConfigShardHost("FakeConfigHost", 12345);

const Status kShardNotFoundStatus{ErrorCodes::ShardNotFound, "dummy"};
const Status kError1Status{ErrorCodes::HostUnreachable, "dummy"};
const Status kError2Status{ErrorCodes::HostUnreachable, "dummy"};

const Status kWriteConcernError1Status{ErrorCodes::WriteConcernTimeout, "dummy"};
const Status kWriteConcernError2Status{ErrorCodes::UnsatisfiableWriteConcern, "dummy"};

executor::RemoteCommandResponse kOkResponse =
    executor::RemoteCommandResponse::make_forTest(BSON("ok" << 1), Milliseconds(0));

executor::RemoteCommandResponse makeErrorResponse(const Status& errorStatus) {
    invariant(!errorStatus.isOK());
    BSONObjBuilder res;
    CommandHelpers::appendCommandStatusNoThrow(res, errorStatus);
    return executor::RemoteCommandResponse::make_forTest(res.obj(), Milliseconds(0));
}

executor::RemoteCommandResponse makeWriteConcernErrorResponse(
    const Status& writeConcernErrorStatus) {
    invariant(!writeConcernErrorStatus.isOK());
    BSONObjBuilder res;
    WriteConcernErrorDetail wcError;
    wcError.setStatus(writeConcernErrorStatus);
    res.append("ok", 1);
    res.append("writeConcernError", wcError.toBSON());
    return executor::RemoteCommandResponse::make_forTest(res.obj(), Milliseconds(0));
}

HostAndPort makeHostAndPort(const ShardHandle& shardHandle) {
    return HostAndPort(str::stream() << shardHandle.name() << ":123");
}

AsyncRequestsSender::Response makeTestArsResponse(
    const ShardHandle& shardHandle,
    StatusWith<executor::RemoteCommandResponse> swResponse,
    boost::optional<HostAndPort> hostAndPort = boost::none) {
    return {ShardRef(shardHandle.name()), std::move(swResponse), std::move(hostAndPort)};
}

class AppendRawResponsesTest : public ShardingTestFixture {
protected:
    void setUp() override {
        ShardingTestFixture::setUp();

        configTargeter()->setFindHostReturnValue(kTestConfigShardHost);

        std::vector<std::tuple<ShardId, HostAndPort>> remoteShards;
        for (const auto& shardHandle : kShardHandleList) {
            remoteShards.emplace_back(shardHandle.name(), makeHostAndPort(shardHandle));
        }

        addRemoteShards(remoteShards);
    }

    /**
     * Runs 'appendRawResponses' and asserts that the top-level status matches 'expectedStatus', the
     * 'raw' sub-object contains the shards in 'expectedShardsInRawSubObj', and the writeConcern
     * status matches 'expectedWriteConcernStatus'.
     */
    void runAppendRawResponsesExpect(
        const std::vector<AsyncRequestsSender::Response>& shardResponses,
        const Status& expectedStatus,
        const std::vector<ShardHandle>& expectedShardsInRawSubObj,
        const std::vector<ShardHandle>& expectedShardsWithSuccessResponses,
        const Status& expectedWriteConcernStatus = Status::OK()) {
        BSONObjBuilder result;
        std::string errmsg;

        const auto response =
            appendRawResponses(operationContext(), &errmsg, &result, shardResponses);

        if (!errmsg.empty()) {
            CommandHelpers::appendSimpleCommandStatus(result, response.responseOK, errmsg);
        }
        const auto resultObj = result.obj();

        // Check the top-level status.
        if (expectedStatus.isOK()) {
            ASSERT_FALSE(resultObj.hasField("code"));
            ASSERT_FALSE(resultObj.hasField("codeName"));
            ASSERT(errmsg.empty());
        } else {
            ASSERT_EQ(expectedStatus.code(), resultObj.getField("code").Int());
            ASSERT_EQ(ErrorCodes::errorString(expectedStatus.code()),
                      resultObj.getField("codeName").String());
            ASSERT_EQ(expectedStatus.reason(), errmsg);
        }

        // Check the 'raw' sub-object.
        const auto rawSubObj = resultObj.getField("raw").Obj();
        ASSERT_EQ(rawSubObj.nFields(), int(expectedShardsInRawSubObj.size()));
        for (const auto& shard : expectedShardsInRawSubObj) {
            ASSERT(rawSubObj.hasField(makeHostAndPort(shard).toString()));
        }

        // Check the shards with successes object.
        std::set<ShardId> expectedShardIds;
        for (const auto& shard : expectedShardsWithSuccessResponses) {
            expectedShardIds.insert(shard.name());
        }
        _assertShardIdsMatch(expectedShardIds, response.shardsWithSuccessResponses);

        ASSERT_EQ(expectedShardIds.size(), response.successResponses.size());

        // Check for a writeConcern error.
        if (expectedWriteConcernStatus.isOK()) {
            ASSERT(resultObj.getField("writeConcernError").eoo());
        } else {
            ASSERT_EQ(expectedWriteConcernStatus,
                      getWriteConcernStatusFromCommandResult(resultObj));
        }
    }

    std::unique_ptr<ShardingCatalogClient> makeShardingCatalogClient() override {

        class StaticCatalogClient final : public ShardingCatalogClientMock {
        public:
            StaticCatalogClient(std::vector<ShardHandle> shards)
                : _shardHandles(std::move(shards)) {}

            repl::OpTimeWith<std::vector<ShardType>> getAllShards(
                OperationContext* opCtx,
                repl::ReadConcernLevel readConcern,
                BSONObj filter) override {
                std::vector<ShardType> shardTypes;
                for (const auto& shardHandle : _shardHandles) {
                    const ConnectionString cs = ConnectionString::forReplicaSet(
                        shardHandle.name().toString(), {makeHostAndPort(shardHandle)});
                    ShardType sType;
                    sType.setHandle(shardHandle);
                    sType.setHost(cs.toString());
                    shardTypes.push_back(std::move(sType));
                };
                return repl::OpTimeWith<std::vector<ShardType>>(shardTypes);
            }

        private:
            const std::vector<ShardHandle> _shardHandles;
        };

        return std::make_unique<StaticCatalogClient>(kShardHandleList);
    }

    const ShardHandle kShardHandle1{ShardId("s1"), boost::make_optional(UUID::gen())};
    const ShardHandle kShardHandle2{ShardId("s2"), boost::make_optional(UUID::gen())};
    const ShardHandle kShardHandle3{ShardId("s3"), boost::make_optional(UUID::gen())};
    const ShardHandle kShardHandle4{ShardId("s4"), boost::make_optional(UUID::gen())};
    const ShardHandle kShardHandle5{ShardId("s5"), boost::make_optional(UUID::gen())};

    const std::vector<ShardHandle> kShardHandleList{
        kShardHandle1, kShardHandle2, kShardHandle3, kShardHandle4, kShardHandle5};

private:
    static void _assertShardIdsMatch(const std::set<ShardId>& expectedShardIds,
                                     const std::set<ShardId>& actualShardIds) {
        BSONArrayBuilder expectedBuilder;
        for (const auto& shardId : expectedShardIds) {
            expectedBuilder << shardId;
        }

        BSONArrayBuilder actualBuilder;
        for (const auto& shardId : actualShardIds) {
            actualBuilder << shardId;
        }

        ASSERT_BSONOBJ_EQ(expectedBuilder.arr(), actualBuilder.arr());
    }
};

//
// No errors
//

TEST_F(AppendRawResponsesTest, AllShardsReturnSuccess) {
    std::vector<AsyncRequestsSender::Response> shardResponses{
        makeTestArsResponse(kShardHandle1, kOkResponse, makeHostAndPort(kShardHandle1)),
        makeTestArsResponse(kShardHandle2, kOkResponse, makeHostAndPort(kShardHandle2)),
        makeTestArsResponse(kShardHandle3, kOkResponse, makeHostAndPort(kShardHandle3))};

    runAppendRawResponsesExpect(shardResponses,
                                Status::OK(),
                                {kShardHandle1, kShardHandle2, kShardHandle3},
                                {kShardHandle1, kShardHandle2, kShardHandle3});
}

//
// Only write concern errors
//

TEST_F(AppendRawResponsesTest, AllShardsReturnSuccessOneWithWriteConcernError) {
    std::vector<AsyncRequestsSender::Response> shardResponses{
        makeTestArsResponse(kShardHandle1,
                            makeWriteConcernErrorResponse(kWriteConcernError1Status),
                            makeHostAndPort(kShardHandle1)),
        makeTestArsResponse(kShardHandle2, kOkResponse, makeHostAndPort(kShardHandle2)),
        makeTestArsResponse(kShardHandle3, kOkResponse, makeHostAndPort(kShardHandle3))};

    runAppendRawResponsesExpect(shardResponses,
                                Status::OK(),
                                {kShardHandle1, kShardHandle2, kShardHandle3},
                                {kShardHandle1, kShardHandle2, kShardHandle3},
                                kWriteConcernError1Status);
}

TEST_F(AppendRawResponsesTest, AllShardsReturnSuccessMoreThanOneWithWriteConcernError) {
    std::vector<AsyncRequestsSender::Response> shardResponses{
        makeTestArsResponse(kShardHandle1,
                            makeWriteConcernErrorResponse(kWriteConcernError1Status),
                            makeHostAndPort(kShardHandle1)),
        makeTestArsResponse(kShardHandle2,
                            makeWriteConcernErrorResponse(kWriteConcernError2Status),
                            makeHostAndPort(kShardHandle2)),
        makeTestArsResponse(kShardHandle3, kOkResponse, makeHostAndPort(kShardHandle3))};

    // The first writeConcern error is reported.
    runAppendRawResponsesExpect(shardResponses,
                                Status::OK(),
                                {kShardHandle1, kShardHandle2, kShardHandle3},
                                {kShardHandle1, kShardHandle2, kShardHandle3},
                                kWriteConcernError1Status);
}

TEST_F(AppendRawResponsesTest, AllShardsReturnSuccessAllWithWriteConcernError) {
    std::vector<AsyncRequestsSender::Response> shardResponses{
        makeTestArsResponse(kShardHandle1,
                            makeWriteConcernErrorResponse(kWriteConcernError1Status),
                            makeHostAndPort(kShardHandle1)),
        makeTestArsResponse(kShardHandle2,
                            makeWriteConcernErrorResponse(kWriteConcernError2Status),
                            makeHostAndPort(kShardHandle2)),
        makeTestArsResponse(kShardHandle3,
                            makeWriteConcernErrorResponse(kWriteConcernError1Status),
                            makeHostAndPort(kShardHandle3))};

    // The first writeConcern error is reported.
    runAppendRawResponsesExpect(shardResponses,
                                Status::OK(),
                                {kShardHandle1, kShardHandle2, kShardHandle3},
                                {kShardHandle1, kShardHandle2, kShardHandle3},
                                kWriteConcernError1Status);
}

//
// Errors *sending* the requests.
//

TEST_F(AppendRawResponsesTest, AllAttemptsToSendRequestsReturnShardNotFoundError) {
    std::vector<AsyncRequestsSender::Response> shardResponses{
        makeTestArsResponse(kShardHandle1, kShardNotFoundStatus, boost::none),
        makeTestArsResponse(kShardHandle2, kShardNotFoundStatus, boost::none),
        makeTestArsResponse(kShardHandle3, kShardNotFoundStatus, boost::none)};

    // The ShardNotFound error gets promoted to a regular error.
    runAppendRawResponsesExpect(
        shardResponses, kShardNotFoundStatus, {kShardHandle1, kShardHandle2, kShardHandle3}, {});
}

TEST_F(AppendRawResponsesTest,
       SomeShardsReturnSuccessRestOfAttemptsToSendRequestsReturnShardNotFoundError) {
    std::vector<AsyncRequestsSender::Response> shardResponses{
        makeTestArsResponse(kShardHandle1, kOkResponse, makeHostAndPort(kShardHandle1)),
        makeTestArsResponse(kShardHandle2, kOkResponse, makeHostAndPort(kShardHandle2)),
        makeTestArsResponse(kShardHandle3, kShardNotFoundStatus, boost::none),
        makeTestArsResponse(kShardHandle4, kShardNotFoundStatus, boost::none)};

    // The ShardNotFound errors are ignored.
    runAppendRawResponsesExpect(shardResponses,
                                Status::OK(),
                                {kShardHandle1, kShardHandle2},
                                {kShardHandle1, kShardHandle2});
}

TEST_F(AppendRawResponsesTest, AllAttemptsToSendRequestsReturnSameError) {
    std::vector<AsyncRequestsSender::Response> shardResponses{
        makeTestArsResponse(kShardHandle1, kError1Status, boost::none),
        makeTestArsResponse(kShardHandle2, kError1Status, boost::none),
        makeTestArsResponse(kShardHandle3, kError1Status, boost::none)};

    // The error is returned.
    runAppendRawResponsesExpect(
        shardResponses, kError1Status, {kShardHandle1, kShardHandle2, kShardHandle3}, {});
}

TEST_F(AppendRawResponsesTest, SomeShardsReturnSuccessRestOfAttemptsToSendRequestsReturnSameError) {
    std::vector<AsyncRequestsSender::Response> shardResponses{
        makeTestArsResponse(kShardHandle1, kOkResponse, makeHostAndPort(kShardHandle1)),
        makeTestArsResponse(kShardHandle2, kOkResponse, makeHostAndPort(kShardHandle2)),
        makeTestArsResponse(kShardHandle3, kError1Status, boost::none),
        makeTestArsResponse(kShardHandle4, kError1Status, boost::none)};

    // The error is returned.
    runAppendRawResponsesExpect(shardResponses,
                                kError1Status,
                                {kShardHandle1, kShardHandle2, kShardHandle3, kShardHandle4},
                                {kShardHandle1, kShardHandle2});
}

TEST_F(AppendRawResponsesTest, AttemptsToSendRequestsReturnDifferentErrors) {
    std::vector<AsyncRequestsSender::Response> shardResponses{
        makeTestArsResponse(kShardHandle1, kError1Status, boost::none),
        makeTestArsResponse(kShardHandle2, kError2Status, boost::none),
        makeTestArsResponse(kShardHandle3, kError1Status, boost::none)};

    // The first error is returned.
    runAppendRawResponsesExpect(
        shardResponses, kError1Status, {kShardHandle1, kShardHandle2, kShardHandle3}, {});
}

TEST_F(AppendRawResponsesTest,
       SomeShardsReturnSuccessRestOfAttemptsToSendRequestsReturnDifferentErrors) {
    std::vector<AsyncRequestsSender::Response> shardResponses{
        makeTestArsResponse(kShardHandle1, kOkResponse, makeHostAndPort(kShardHandle1)),
        makeTestArsResponse(kShardHandle2, kOkResponse, makeHostAndPort(kShardHandle2)),
        makeTestArsResponse(kShardHandle3, kError1Status, boost::none),
        makeTestArsResponse(kShardHandle4, kError2Status, boost::none)};

    // The first error is returned.
    runAppendRawResponsesExpect(shardResponses,
                                kError1Status,
                                {kShardHandle1, kShardHandle2, kShardHandle3, kShardHandle4},
                                {kShardHandle1, kShardHandle2});
}

TEST_F(AppendRawResponsesTest, AllAttemptsToSendRequestsReturnErrorsSomeShardNotFound) {
    std::vector<AsyncRequestsSender::Response> shardResponses{
        makeTestArsResponse(kShardHandle1, kShardNotFoundStatus, boost::none),
        makeTestArsResponse(kShardHandle2, kShardNotFoundStatus, boost::none),
        makeTestArsResponse(kShardHandle3, kError1Status, boost::none),
        makeTestArsResponse(kShardHandle4, kError2Status, boost::none)};

    // The first error is returned.
    runAppendRawResponsesExpect(shardResponses, kError1Status, {kShardHandle3, kShardHandle4}, {});
}

TEST_F(AppendRawResponsesTest,
       SomeShardsReturnSuccessRestOfAttemptsToSendRequestsReturnErrorsSomeShardNotFound) {
    std::vector<AsyncRequestsSender::Response> shardResponses{
        makeTestArsResponse(kShardHandle1, kShardNotFoundStatus, boost::none),
        makeTestArsResponse(kShardHandle2, kOkResponse, makeHostAndPort(kShardHandle2)),
        makeTestArsResponse(kShardHandle3, kError1Status, boost::none),
        makeTestArsResponse(kShardHandle4, kError1Status, boost::none)};

    // The first error is returned.
    runAppendRawResponsesExpect(shardResponses,
                                kError1Status,
                                {kShardHandle2, kShardHandle3, kShardHandle4},
                                {kShardHandle2});
}

//
// Errors *processing* the requests.
//

TEST_F(AppendRawResponsesTest, AllShardsReturnShardNotFoundError) {
    std::vector<AsyncRequestsSender::Response> shardResponses{
        makeTestArsResponse(
            kShardHandle1, makeErrorResponse(kShardNotFoundStatus), makeHostAndPort(kShardHandle1)),
        makeTestArsResponse(
            kShardHandle2, makeErrorResponse(kShardNotFoundStatus), makeHostAndPort(kShardHandle2)),
        makeTestArsResponse(kShardHandle3,
                            makeErrorResponse(kShardNotFoundStatus),
                            makeHostAndPort(kShardHandle3))};

    // The ShardNotFound error gets promoted to a regular error.
    runAppendRawResponsesExpect(
        shardResponses, kShardNotFoundStatus, {kShardHandle1, kShardHandle2, kShardHandle3}, {});
}

TEST_F(AppendRawResponsesTest, SomeShardsReturnSuccessRestReturnShardNotFound) {
    std::vector<AsyncRequestsSender::Response> shardResponses{
        makeTestArsResponse(kShardHandle1, kOkResponse, makeHostAndPort(kShardHandle1)),
        makeTestArsResponse(kShardHandle2, kOkResponse, makeHostAndPort(kShardHandle2)),
        makeTestArsResponse(
            kShardHandle3, makeErrorResponse(kShardNotFoundStatus), makeHostAndPort(kShardHandle3)),
        makeTestArsResponse(kShardHandle4,
                            makeErrorResponse(kShardNotFoundStatus),
                            makeHostAndPort(kShardHandle4))};

    // The ShardNotFound errors are ignored.
    runAppendRawResponsesExpect(shardResponses,
                                Status::OK(),
                                {kShardHandle1, kShardHandle2},
                                {kShardHandle1, kShardHandle2});
}

TEST_F(AppendRawResponsesTest, AllShardsReturnSameError) {
    std::vector<AsyncRequestsSender::Response> shardResponses{
        makeTestArsResponse(
            kShardHandle1, makeErrorResponse(kError1Status), makeHostAndPort(kShardHandle1)),
        makeTestArsResponse(
            kShardHandle2, makeErrorResponse(kError1Status), makeHostAndPort(kShardHandle2)),
        makeTestArsResponse(
            kShardHandle3, makeErrorResponse(kError1Status), makeHostAndPort(kShardHandle3))};

    // The error is returned.
    runAppendRawResponsesExpect(
        shardResponses, kError1Status, {kShardHandle1, kShardHandle2, kShardHandle3}, {});
}

TEST_F(AppendRawResponsesTest, SomeShardsReturnSuccessRestReturnError) {
    std::vector<AsyncRequestsSender::Response> shardResponses{
        makeTestArsResponse(kShardHandle1, kOkResponse, makeHostAndPort(kShardHandle1)),
        makeTestArsResponse(kShardHandle2, kOkResponse, makeHostAndPort(kShardHandle2)),
        makeTestArsResponse(
            kShardHandle3, makeErrorResponse(kError1Status), makeHostAndPort(kShardHandle3)),
        makeTestArsResponse(
            kShardHandle4, makeErrorResponse(kError1Status), makeHostAndPort(kShardHandle4))};

    // The error is returned.
    runAppendRawResponsesExpect(shardResponses,
                                kError1Status,
                                {kShardHandle1, kShardHandle2, kShardHandle3, kShardHandle4},
                                {kShardHandle1, kShardHandle2});
}

TEST_F(AppendRawResponsesTest, ShardsReturnDifferentErrors) {
    std::vector<AsyncRequestsSender::Response> shardResponses{
        makeTestArsResponse(
            kShardHandle1, makeErrorResponse(kError1Status), makeHostAndPort(kShardHandle1)),
        makeTestArsResponse(
            kShardHandle2, makeErrorResponse(kError2Status), makeHostAndPort(kShardHandle2)),
        makeTestArsResponse(
            kShardHandle3, makeErrorResponse(kError1Status), makeHostAndPort(kShardHandle3))};

    // The first error is returned.
    runAppendRawResponsesExpect(
        shardResponses, kError1Status, {kShardHandle1, kShardHandle2, kShardHandle3}, {});
}

TEST_F(AppendRawResponsesTest, SomeShardsReturnSuccessRestReturnDifferentErrors) {
    std::vector<AsyncRequestsSender::Response> shardResponses{
        makeTestArsResponse(kShardHandle1, kOkResponse, makeHostAndPort(kShardHandle1)),
        makeTestArsResponse(kShardHandle2, kOkResponse, makeHostAndPort(kShardHandle2)),
        makeTestArsResponse(
            kShardHandle3, makeErrorResponse(kError1Status), makeHostAndPort(kShardHandle3)),
        makeTestArsResponse(
            kShardHandle4, makeErrorResponse(kError2Status), makeHostAndPort(kShardHandle4))};

    // The first error is returned.
    runAppendRawResponsesExpect(shardResponses,
                                kError1Status,
                                {kShardHandle1, kShardHandle2, kShardHandle3, kShardHandle4},
                                {kShardHandle1, kShardHandle2});
}

TEST_F(AppendRawResponsesTest, AllShardsReturnErrorsSomeShardNotFound) {
    std::vector<AsyncRequestsSender::Response> shardResponses{
        makeTestArsResponse(
            kShardHandle1, makeErrorResponse(kShardNotFoundStatus), makeHostAndPort(kShardHandle1)),
        makeTestArsResponse(
            kShardHandle2, makeErrorResponse(kShardNotFoundStatus), makeHostAndPort(kShardHandle2)),
        makeTestArsResponse(
            kShardHandle3, makeErrorResponse(kError1Status), makeHostAndPort(kShardHandle3)),
        makeTestArsResponse(
            kShardHandle4, makeErrorResponse(kError2Status), makeHostAndPort(kShardHandle4))};

    // The first non-ShardNotFound error is returned.
    runAppendRawResponsesExpect(shardResponses, kError1Status, {kShardHandle3, kShardHandle4}, {});
}

TEST_F(AppendRawResponsesTest, SomeShardsReturnSuccessRestReturnErrorsSomeShardNotFound) {
    std::vector<AsyncRequestsSender::Response> shardResponses{
        makeTestArsResponse(
            kShardHandle1, makeErrorResponse(kShardNotFoundStatus), makeHostAndPort(kShardHandle1)),
        makeTestArsResponse(kShardHandle2, kOkResponse, makeHostAndPort(kShardHandle2)),
        makeTestArsResponse(
            kShardHandle3, makeErrorResponse(kError1Status), makeHostAndPort(kShardHandle3)),
        makeTestArsResponse(
            kShardHandle4, makeErrorResponse(kError2Status), makeHostAndPort(kShardHandle4))};

    // The first non-ShardNotFound error is returned.
    runAppendRawResponsesExpect(shardResponses,
                                kError1Status,
                                {kShardHandle2, kShardHandle3, kShardHandle4},
                                {kShardHandle2});
}

// Mix of errors *sending* and *processing* the requests.

TEST_F(AppendRawResponsesTest,
       AllShardsReturnErrorsMixOfErrorsSendingRequestsAndErrorsProcessingRequests) {
    std::vector<AsyncRequestsSender::Response> shardResponses{
        makeTestArsResponse(kShardHandle1, kShardNotFoundStatus, boost::none),
        makeTestArsResponse(
            kShardHandle2, makeErrorResponse(kShardNotFoundStatus), makeHostAndPort(kShardHandle2)),
        makeTestArsResponse(kShardHandle3, kError1Status, boost::none),
        makeTestArsResponse(
            kShardHandle4, makeErrorResponse(kError2Status), makeHostAndPort(kShardHandle4))};

    // The first non-ShardNotFound error is returned.
    runAppendRawResponsesExpect(shardResponses, kError1Status, {kShardHandle3, kShardHandle4}, {});
}

TEST_F(
    AppendRawResponsesTest,
    SomeShardsReturnSuccessRestReturnErrorsMixOfErrorsSendingRequestsAndErrorsProcessingRequests) {
    std::vector<AsyncRequestsSender::Response> shardResponses{
        makeTestArsResponse(kShardHandle1, kShardNotFoundStatus, boost::none),
        makeTestArsResponse(
            kShardHandle2, makeErrorResponse(kShardNotFoundStatus), makeHostAndPort(kShardHandle2)),
        makeTestArsResponse(kShardHandle3, kError1Status, boost::none),
        makeTestArsResponse(
            kShardHandle4, makeErrorResponse(kError2Status), makeHostAndPort(kShardHandle4)),
        makeTestArsResponse(kShardHandle5, kOkResponse, HostAndPort("e:1"))};

    // The first non-ShardNotFound error is returned.
    runAppendRawResponsesExpect(shardResponses,
                                kError1Status,
                                {kShardHandle3, kShardHandle4, kShardHandle5},
                                {kShardHandle5});
}

// Mix of errors sending or processing the requests *and* writeConcern error.

TEST_F(AppendRawResponsesTest, SomeShardsReturnSuccessWithWriteConcernErrorRestReturnMixOfErrors) {
    std::vector<AsyncRequestsSender::Response> shardResponses{
        makeTestArsResponse(kShardHandle1, kShardNotFoundStatus, boost::none),
        makeTestArsResponse(
            kShardHandle2, makeErrorResponse(kShardNotFoundStatus), makeHostAndPort(kShardHandle2)),
        makeTestArsResponse(kShardHandle3, kError1Status, boost::none),
        makeTestArsResponse(
            kShardHandle4, makeErrorResponse(kError2Status), makeHostAndPort(kShardHandle4)),
        makeTestArsResponse(kShardHandle5,
                            makeWriteConcernErrorResponse(kWriteConcernError1Status),
                            makeHostAndPort(kShardHandle5))};

    // The first non-ShardNotFound error is returned, and writeConcern error is reported at the top
    // level.
    runAppendRawResponsesExpect(shardResponses,
                                kError1Status,
                                {kShardHandle3, kShardHandle4, kShardHandle5},
                                {kShardHandle5},
                                kWriteConcernError1Status);
}

}  // namespace
}  // namespace mongo
