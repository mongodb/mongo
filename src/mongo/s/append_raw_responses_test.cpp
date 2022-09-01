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

#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/commands.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/s/catalog/sharding_catalog_client_mock.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/sharding_router_test_fixture.h"
#include "mongo/unittest/unittest.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo {
namespace {

const HostAndPort kTestConfigShardHost("FakeConfigHost", 12345);

const Status kShardNotFoundStatus{ErrorCodes::ShardNotFound, "dummy"};
const Status kError1Status{ErrorCodes::HostUnreachable, "dummy"};
const Status kError2Status{ErrorCodes::HostUnreachable, "dummy"};

const Status kWriteConcernError1Status{ErrorCodes::WriteConcernFailed, "dummy"};
const Status kWriteConcernError2Status{ErrorCodes::UnsatisfiableWriteConcern, "dummy"};

executor::RemoteCommandResponse kOkResponse{BSON("ok" << 1), Milliseconds(0)};

executor::RemoteCommandResponse makeErrorResponse(const Status& errorStatus) {
    invariant(!errorStatus.isOK());
    BSONObjBuilder res;
    CommandHelpers::appendCommandStatusNoThrow(res, errorStatus);
    return {res.obj(), Milliseconds(0)};
}

executor::RemoteCommandResponse makeWriteConcernErrorResponse(
    const Status& writeConcernErrorStatus) {
    invariant(!writeConcernErrorStatus.isOK());
    BSONObjBuilder res;
    WriteConcernErrorDetail wcError;
    wcError.setStatus(writeConcernErrorStatus);
    res.append("ok", 1);
    res.append("writeConcernError", wcError.toBSON());
    return {res.obj(), Milliseconds(0)};
}

HostAndPort makeHostAndPort(const ShardId& shardId) {
    return HostAndPort(str::stream() << shardId << ":123");
}

class AppendRawResponsesTest : public ShardingTestFixture {
protected:
    void setUp() {
        ShardingTestFixture::setUp();

        configTargeter()->setFindHostReturnValue(kTestConfigShardHost);

        std::vector<std::tuple<ShardId, HostAndPort>> remoteShards;
        for (const auto& shardId : kShardIdList) {
            remoteShards.emplace_back(shardId, makeHostAndPort(shardId));
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
        const std::vector<ShardId>& expectedShardsInRawSubObj,
        const std::set<ShardId>& expectedShardsWithSuccessResponses,
        const Status& expectedWriteConcernStatus = Status::OK(),
        bool expectsStaleConfigError = false) {
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
        for (const auto& shardId : expectedShardsInRawSubObj) {
            ASSERT(rawSubObj.hasField(makeHostAndPort(shardId).toString()));
        }

        // Check the shards with successes object.
        _assertShardIdsMatch(expectedShardsWithSuccessResponses,
                             response.shardsWithSuccessResponses);

        ASSERT_EQ(expectedShardsWithSuccessResponses.size(), response.successResponses.size());

        // Check for a writeConcern error.
        if (expectedWriteConcernStatus.isOK()) {
            ASSERT(resultObj.getField("writeConcernError").eoo());
        } else {
            ASSERT_EQ(expectedWriteConcernStatus,
                      getWriteConcernStatusFromCommandResult(resultObj));
        }

        // Check that if we had a StaleConfigError, it was placed in the response.
        if (expectsStaleConfigError) {
            ASSERT(response.firstStaleConfigError);
            ASSERT_EQ(response.firstStaleConfigError->code(), kStaleConfigErrorStatus.code());
            ASSERT_EQ(response.firstStaleConfigError->reason(), kStaleConfigErrorStatus.reason());
        } else {
            ASSERT_FALSE(response.firstStaleConfigError);
        }
    }

    std::unique_ptr<ShardingCatalogClient> makeShardingCatalogClient() override {

        class StaticCatalogClient final : public ShardingCatalogClientMock {
        public:
            StaticCatalogClient(std::vector<ShardId> shardIds) : _shardIds(std::move(shardIds)) {}

            StatusWith<repl::OpTimeWith<std::vector<ShardType>>> getAllShards(
                OperationContext* opCtx, repl::ReadConcernLevel readConcern) override {
                std::vector<ShardType> shardTypes;
                for (const auto& shardId : _shardIds) {
                    const ConnectionString cs = ConnectionString::forReplicaSet(
                        shardId.toString(), {makeHostAndPort(shardId)});
                    ShardType sType;
                    sType.setName(cs.getSetName());
                    sType.setHost(cs.toString());
                    shardTypes.push_back(std::move(sType));
                };
                return repl::OpTimeWith<std::vector<ShardType>>(shardTypes);
            }

        private:
            const std::vector<ShardId> _shardIds;
        };

        return std::make_unique<StaticCatalogClient>(kShardIdList);
    }

    const ShardId kShard1{"s1"};
    const ShardId kShard2{"s2"};
    const ShardId kShard3{"s3"};
    const ShardId kShard4{"s4"};
    const ShardId kShard5{"s5"};

    const std::vector<ShardId> kShardIdList{kShard1, kShard2, kShard3, kShard4, kShard5};

    const Status kStaleConfigErrorStatus{
        [] {
            OID epoch{OID::gen()};
            Timestamp timestamp{1, 0};
            return StaleConfigInfo(NamespaceString("Foo.Bar"),
                                   ShardVersion(ChunkVersion({epoch, timestamp}, {1, 0}),
                                                CollectionIndexes({epoch, timestamp}, boost::none)),
                                   boost::none,
                                   ShardId{"dummy"});
        }(),
        "dummy"};

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
        {kShard1, kOkResponse, makeHostAndPort(kShard1)},
        {kShard2, kOkResponse, makeHostAndPort(kShard2)},
        {kShard3, kOkResponse, makeHostAndPort(kShard3)}};

    runAppendRawResponsesExpect(
        shardResponses, Status::OK(), {kShard1, kShard2, kShard3}, {kShard1, kShard2, kShard3});
}

//
// Only write concern errors
//

TEST_F(AppendRawResponsesTest, AllShardsReturnSuccessOneWithWriteConcernError) {
    std::vector<AsyncRequestsSender::Response> shardResponses{
        {kShard1,
         makeWriteConcernErrorResponse(kWriteConcernError1Status),
         makeHostAndPort(kShard1)},
        {kShard2, kOkResponse, makeHostAndPort(kShard2)},
        {kShard3, kOkResponse, makeHostAndPort(kShard3)}};

    runAppendRawResponsesExpect(shardResponses,
                                Status::OK(),
                                {kShard1, kShard2, kShard3},
                                {kShard1, kShard2, kShard3},
                                kWriteConcernError1Status);
}

TEST_F(AppendRawResponsesTest, AllShardsReturnSuccessMoreThanOneWithWriteConcernError) {
    std::vector<AsyncRequestsSender::Response> shardResponses{
        {kShard1,
         makeWriteConcernErrorResponse(kWriteConcernError1Status),
         makeHostAndPort(kShard1)},
        {kShard2,
         makeWriteConcernErrorResponse(kWriteConcernError2Status),
         makeHostAndPort(kShard2)},
        {kShard3, kOkResponse, makeHostAndPort(kShard3)}};

    // The first writeConcern error is reported.
    runAppendRawResponsesExpect(shardResponses,
                                Status::OK(),
                                {kShard1, kShard2, kShard3},
                                {kShard1, kShard2, kShard3},
                                kWriteConcernError1Status);
}

TEST_F(AppendRawResponsesTest, AllShardsReturnSuccessAllWithWriteConcernError) {
    std::vector<AsyncRequestsSender::Response> shardResponses{
        {kShard1,
         makeWriteConcernErrorResponse(kWriteConcernError1Status),
         makeHostAndPort(kShard1)},
        {kShard2,
         makeWriteConcernErrorResponse(kWriteConcernError2Status),
         makeHostAndPort(kShard2)},
        {kShard3,
         makeWriteConcernErrorResponse(kWriteConcernError1Status),
         makeHostAndPort(kShard3)}};

    // The first writeConcern error is reported.
    runAppendRawResponsesExpect(shardResponses,
                                Status::OK(),
                                {kShard1, kShard2, kShard3},
                                {kShard1, kShard2, kShard3},
                                kWriteConcernError1Status);
}

//
// Errors *sending* the requests.
//

TEST_F(AppendRawResponsesTest, AllAttemptsToSendRequestsReturnShardNotFoundError) {
    std::vector<AsyncRequestsSender::Response> shardResponses{
        {kShard1, kShardNotFoundStatus, boost::none},
        {kShard2, kShardNotFoundStatus, boost::none},
        {kShard3, kShardNotFoundStatus, boost::none}};

    // The ShardNotFound error gets promoted to a regular error.
    runAppendRawResponsesExpect(
        shardResponses, kShardNotFoundStatus, {kShard1, kShard2, kShard3}, {});
}

TEST_F(AppendRawResponsesTest,
       SomeShardsReturnSuccessRestOfAttemptsToSendRequestsReturnShardNotFoundError) {
    std::vector<AsyncRequestsSender::Response> shardResponses{
        {kShard1, kOkResponse, makeHostAndPort(kShard1)},
        {kShard2, kOkResponse, makeHostAndPort(kShard2)},
        {kShard3, kShardNotFoundStatus, boost::none},
        {kShard4, kShardNotFoundStatus, boost::none}};

    // The ShardNotFound errors are ignored.
    runAppendRawResponsesExpect(
        shardResponses, Status::OK(), {kShard1, kShard2}, {kShard1, kShard2});
}

TEST_F(AppendRawResponsesTest, AllAttemptsToSendRequestsReturnSameError) {
    std::vector<AsyncRequestsSender::Response> shardResponses{
        {kShard1, kError1Status, boost::none},
        {kShard2, kError1Status, boost::none},
        {kShard3, kError1Status, boost::none}};

    // The error is returned.
    runAppendRawResponsesExpect(shardResponses, kError1Status, {kShard1, kShard2, kShard3}, {});
}

TEST_F(AppendRawResponsesTest, SomeShardsReturnSuccessRestOfAttemptsToSendRequestsReturnSameError) {
    std::vector<AsyncRequestsSender::Response> shardResponses{
        {kShard1, kOkResponse, makeHostAndPort(kShard1)},
        {kShard2, kOkResponse, makeHostAndPort(kShard2)},
        {kShard3, kError1Status, boost::none},
        {kShard4, kError1Status, boost::none}};

    // The error is returned.
    runAppendRawResponsesExpect(
        shardResponses, kError1Status, {kShard1, kShard2, kShard3, kShard4}, {kShard1, kShard2});
}

TEST_F(AppendRawResponsesTest, AttemptsToSendRequestsReturnDifferentErrors) {
    std::vector<AsyncRequestsSender::Response> shardResponses{
        {kShard1, kError1Status, boost::none},
        {kShard2, kError2Status, boost::none},
        {kShard3, kError1Status, boost::none}};

    // The first error is returned.
    runAppendRawResponsesExpect(shardResponses, kError1Status, {kShard1, kShard2, kShard3}, {});
}

TEST_F(AppendRawResponsesTest,
       SomeShardsReturnSuccessRestOfAttemptsToSendRequestsReturnDifferentErrors) {
    std::vector<AsyncRequestsSender::Response> shardResponses{
        {kShard1, kOkResponse, makeHostAndPort(kShard1)},
        {kShard2, kOkResponse, makeHostAndPort(kShard2)},
        {kShard3, kError1Status, boost::none},
        {kShard4, kError2Status, boost::none}};

    // The first error is returned.
    runAppendRawResponsesExpect(
        shardResponses, kError1Status, {kShard1, kShard2, kShard3, kShard4}, {kShard1, kShard2});
}

TEST_F(AppendRawResponsesTest, AllAttemptsToSendRequestsReturnErrorsSomeShardNotFound) {
    std::vector<AsyncRequestsSender::Response> shardResponses{
        {kShard1, kShardNotFoundStatus, boost::none},
        {kShard2, kShardNotFoundStatus, boost::none},
        {kShard3, kError1Status, boost::none},
        {kShard4, kError2Status, boost::none}};

    // The first error is returned.
    runAppendRawResponsesExpect(shardResponses, kError1Status, {kShard3, kShard4}, {});
}

TEST_F(AppendRawResponsesTest,
       SomeShardsReturnSuccessRestOfAttemptsToSendRequestsReturnErrorsSomeShardNotFound) {
    std::vector<AsyncRequestsSender::Response> shardResponses{
        {kShard1, kShardNotFoundStatus, boost::none},
        {kShard2, kOkResponse, makeHostAndPort(kShard2)},
        {kShard3, kError1Status, boost::none},
        {kShard4, kError1Status, boost::none}};

    // The first error is returned.
    runAppendRawResponsesExpect(
        shardResponses, kError1Status, {kShard2, kShard3, kShard4}, {kShard2});
}

//
// Errors *processing* the requests.
//

TEST_F(AppendRawResponsesTest, AllShardsReturnShardNotFoundError) {
    std::vector<AsyncRequestsSender::Response> shardResponses{
        {kShard1, makeErrorResponse(kShardNotFoundStatus), makeHostAndPort(kShard1)},
        {kShard2, makeErrorResponse(kShardNotFoundStatus), makeHostAndPort(kShard2)},
        {kShard3, makeErrorResponse(kShardNotFoundStatus), makeHostAndPort(kShard3)}};

    // The ShardNotFound error gets promoted to a regular error.
    runAppendRawResponsesExpect(
        shardResponses, kShardNotFoundStatus, {kShard1, kShard2, kShard3}, {});
}

TEST_F(AppendRawResponsesTest, SomeShardsReturnSuccessRestReturnShardNotFound) {
    std::vector<AsyncRequestsSender::Response> shardResponses{
        {kShard1, kOkResponse, makeHostAndPort(kShard1)},
        {kShard2, kOkResponse, makeHostAndPort(kShard2)},
        {kShard3, makeErrorResponse(kShardNotFoundStatus), makeHostAndPort(kShard3)},
        {kShard4, makeErrorResponse(kShardNotFoundStatus), makeHostAndPort(kShard4)}};

    // The ShardNotFound errors are ignored.
    runAppendRawResponsesExpect(
        shardResponses, Status::OK(), {kShard1, kShard2}, {kShard1, kShard2});
}

TEST_F(AppendRawResponsesTest, AllShardsReturnSameError) {
    std::vector<AsyncRequestsSender::Response> shardResponses{
        {kShard1, makeErrorResponse(kError1Status), makeHostAndPort(kShard1)},
        {kShard2, makeErrorResponse(kError1Status), makeHostAndPort(kShard2)},
        {kShard3, makeErrorResponse(kError1Status), makeHostAndPort(kShard3)}};

    // The error is returned.
    runAppendRawResponsesExpect(shardResponses, kError1Status, {kShard1, kShard2, kShard3}, {});
}

TEST_F(AppendRawResponsesTest, SomeShardsReturnSuccessRestReturnError) {
    std::vector<AsyncRequestsSender::Response> shardResponses{
        {kShard1, kOkResponse, makeHostAndPort(kShard1)},
        {kShard2, kOkResponse, makeHostAndPort(kShard2)},
        {kShard3, makeErrorResponse(kError1Status), makeHostAndPort(kShard3)},
        {kShard4, makeErrorResponse(kError1Status), makeHostAndPort(kShard4)}};

    // The error is returned.
    runAppendRawResponsesExpect(
        shardResponses, kError1Status, {kShard1, kShard2, kShard3, kShard4}, {kShard1, kShard2});
}

TEST_F(AppendRawResponsesTest, ShardsReturnDifferentErrors) {
    std::vector<AsyncRequestsSender::Response> shardResponses{
        {kShard1, makeErrorResponse(kError1Status), makeHostAndPort(kShard1)},
        {kShard2, makeErrorResponse(kError2Status), makeHostAndPort(kShard2)},
        {kShard3, makeErrorResponse(kError1Status), makeHostAndPort(kShard3)}};

    // The first error is returned.
    runAppendRawResponsesExpect(shardResponses, kError1Status, {kShard1, kShard2, kShard3}, {});
}

TEST_F(AppendRawResponsesTest, ShardsReturnDifferentErrorsIncludingStaleConfig) {
    std::vector<AsyncRequestsSender::Response> shardResponses{
        {kShard1, makeErrorResponse(kError1Status), makeHostAndPort(kShard1)},
        {kShard2, makeErrorResponse(kError2Status), makeHostAndPort(kShard2)},
        {kShard3, makeErrorResponse(kStaleConfigErrorStatus), makeHostAndPort(kShard3)}};

    // The first error is returned.
    runAppendRawResponsesExpect(
        shardResponses, kError1Status, {kShard1, kShard2, kShard3}, {}, Status::OK(), true);
}

TEST_F(AppendRawResponsesTest, SomeShardsReturnSuccessRestReturnDifferentErrors) {
    std::vector<AsyncRequestsSender::Response> shardResponses{
        {kShard1, kOkResponse, makeHostAndPort(kShard1)},
        {kShard2, kOkResponse, makeHostAndPort(kShard2)},
        {kShard3, makeErrorResponse(kError1Status), makeHostAndPort(kShard3)},
        {kShard4, makeErrorResponse(kError2Status), makeHostAndPort(kShard4)}};

    // The first error is returned.
    runAppendRawResponsesExpect(
        shardResponses, kError1Status, {kShard1, kShard2, kShard3, kShard4}, {kShard1, kShard2});
}

TEST_F(AppendRawResponsesTest,
       SomeShardsReturnSuccessRestReturnDifferentErrorsIncludingStaleConfig) {
    std::vector<AsyncRequestsSender::Response> shardResponses{
        {kShard1, kOkResponse, makeHostAndPort(kShard1)},
        {kShard2, kOkResponse, makeHostAndPort(kShard2)},
        {kShard3, makeErrorResponse(kError1Status), makeHostAndPort(kShard3)},
        {kShard4, makeErrorResponse(kStaleConfigErrorStatus), makeHostAndPort(kShard4)}};

    // The first error is returned.
    runAppendRawResponsesExpect(shardResponses,
                                kError1Status,
                                {kShard1, kShard2, kShard3, kShard4},
                                {kShard1, kShard2},
                                Status::OK(),
                                true);
}

TEST_F(AppendRawResponsesTest, AllShardsReturnErrorsSomeShardNotFound) {
    std::vector<AsyncRequestsSender::Response> shardResponses{
        {kShard1, makeErrorResponse(kShardNotFoundStatus), makeHostAndPort(kShard1)},
        {kShard2, makeErrorResponse(kShardNotFoundStatus), makeHostAndPort(kShard2)},
        {kShard3, makeErrorResponse(kError1Status), makeHostAndPort(kShard3)},
        {kShard4, makeErrorResponse(kError2Status), makeHostAndPort(kShard4)}};

    // The first non-ShardNotFound error is returned.
    runAppendRawResponsesExpect(shardResponses, kError1Status, {kShard3, kShard4}, {});
}

TEST_F(AppendRawResponsesTest, SomeShardsReturnSuccessRestReturnErrorsSomeShardNotFound) {
    std::vector<AsyncRequestsSender::Response> shardResponses{
        {kShard1, makeErrorResponse(kShardNotFoundStatus), makeHostAndPort(kShard1)},
        {kShard2, kOkResponse, makeHostAndPort(kShard2)},
        {kShard3, makeErrorResponse(kError1Status), makeHostAndPort(kShard3)},
        {kShard4, makeErrorResponse(kError2Status), makeHostAndPort(kShard4)}};

    // The first non-ShardNotFound error is returned.
    runAppendRawResponsesExpect(
        shardResponses, kError1Status, {kShard2, kShard3, kShard4}, {kShard2});
}

// Mix of errors *sending* and *processing* the requests.

TEST_F(AppendRawResponsesTest,
       AllShardsReturnErrorsMixOfErrorsSendingRequestsAndErrorsProcessingRequests) {
    std::vector<AsyncRequestsSender::Response> shardResponses{
        {kShard1, kShardNotFoundStatus, boost::none},
        {kShard2, makeErrorResponse(kShardNotFoundStatus), makeHostAndPort(kShard2)},
        {kShard3, kError1Status, boost::none},
        {kShard4, makeErrorResponse(kError2Status), makeHostAndPort(kShard4)}};

    // The first non-ShardNotFound error is returned.
    runAppendRawResponsesExpect(shardResponses, kError1Status, {kShard3, kShard4}, {});
}

TEST_F(
    AppendRawResponsesTest,
    SomeShardsReturnSuccessRestReturnErrorsMixOfErrorsSendingRequestsAndErrorsProcessingRequests) {
    std::vector<AsyncRequestsSender::Response> shardResponses{
        {kShard1, kShardNotFoundStatus, boost::none},
        {kShard2, makeErrorResponse(kShardNotFoundStatus), makeHostAndPort(kShard2)},
        {kShard3, kError1Status, boost::none},
        {kShard4, makeErrorResponse(kError2Status), makeHostAndPort(kShard4)},
        {kShard5, kOkResponse, HostAndPort("e:1")}};

    // The first non-ShardNotFound error is returned.
    runAppendRawResponsesExpect(
        shardResponses, kError1Status, {kShard3, kShard4, kShard5}, {kShard5});
}

// Mix of errors sending or processing the requests *and* writeConcern error.

TEST_F(AppendRawResponsesTest, SomeShardsReturnSuccessWithWriteConcernErrorRestReturnMixOfErrors) {
    std::vector<AsyncRequestsSender::Response> shardResponses{
        {kShard1, kShardNotFoundStatus, boost::none},
        {kShard2, makeErrorResponse(kShardNotFoundStatus), makeHostAndPort(kShard2)},
        {kShard3, kError1Status, boost::none},
        {kShard4, makeErrorResponse(kError2Status), makeHostAndPort(kShard4)},
        {kShard5,
         makeWriteConcernErrorResponse(kWriteConcernError1Status),
         makeHostAndPort(kShard5)}};

    // The first non-ShardNotFound error is returned, and no writeConcern error is reported at the
    // top level.
    runAppendRawResponsesExpect(
        shardResponses, kError1Status, {kShard3, kShard4, kShard5}, {kShard5});
}

TEST_F(AppendRawResponsesTest,
       SomeShardsReturnSuccessWithWriteConcernErrorRestReturnMixOfErrorsIncludingStaleConfig) {
    std::vector<AsyncRequestsSender::Response> shardResponses{
        {kShard1, kShardNotFoundStatus, boost::none},
        {kShard2, makeErrorResponse(kShardNotFoundStatus), makeHostAndPort(kShard2)},
        {kShard3, kError1Status, boost::none},
        {kShard4, makeErrorResponse(kStaleConfigErrorStatus), makeHostAndPort(kShard4)},
        {kShard5,
         makeWriteConcernErrorResponse(kWriteConcernError1Status),
         makeHostAndPort(kShard5)}};

    // The first non-ShardNotFound error is returned, and no writeConcern error is reported at the
    // top level.
    runAppendRawResponsesExpect(
        shardResponses, kError1Status, {kShard3, kShard4, kShard5}, {kShard5}, Status::OK(), true);
}

}  // namespace
}  // namespace mongo
