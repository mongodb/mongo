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
#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/unittest/unittest.h"

#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/commands.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/s/catalog/sharding_catalog_client_mock.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/shard_server_test_fixture.h"

namespace mongo {
namespace {

// Have two each of ignorable and non-ignorable errors, so we can test shards returning different
// ignorable and different non-ignorable errors.
const ErrorCodes::Error kIgnorableError1{ErrorCodes::NamespaceNotFound};
const ErrorCodes::Error kIgnorableError2{ErrorCodes::NamespaceNotFound};
const ErrorCodes::Error kNonIgnorableError1{ErrorCodes::HostUnreachable};
const ErrorCodes::Error kNonIgnorableError2{ErrorCodes::HostUnreachable};

const Status kIgnorableError1Status{kIgnorableError1, "dummy"};
const Status kIgnorableError2Status{kIgnorableError2, "dummy"};
const Status kNonIgnorableError1Status{kNonIgnorableError1, "dummy"};
const Status kNonIgnorableError2Status{kNonIgnorableError2, "dummy"};

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

}  // namespace

class AppendRawResponsesTest : public ShardServerTestFixture {
protected:
    /**
     * Runs 'appendRawResponses' and asserts that the top-level status matches 'expectedStatus', the
     * 'raw' sub-object contains the shards in 'expectedShardsInRawSubObj', and the writeConcern
     * status matches 'expectedWriteConcernStatus'.
     */
    void runAppendRawResponsesExpect(
        const std::vector<AsyncRequestsSender::Response>& shardResponses,
        const Status& expectedStatus,
        const std::vector<ShardId>& expectedShardsInRawSubObj,
        const Status& expectedWriteConcernStatus = Status::OK()) {
        BSONObjBuilder result;
        std::string errmsg;

        const auto ok = appendRawResponses(operationContext(),
                                           &errmsg,
                                           &result,
                                           shardResponses,
                                           {kIgnorableError1, kIgnorableError2});

        if (!errmsg.empty()) {
            CommandHelpers::appendSimpleCommandStatus(result, ok, errmsg);
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
            const auto shardConnStr =
                ConnectionString::forReplicaSet(shardId.toString(), {makeHostAndPort(shardId)})
                    .toString();
            ASSERT(rawSubObj.hasField(shardConnStr));
        }

        // Check for a writeConcern error.
        if (expectedWriteConcernStatus.isOK()) {
            ASSERT(resultObj.getField("writeConcernError").eoo());
        } else {
            ASSERT_EQ(expectedWriteConcernStatus,
                      getWriteConcernStatusFromCommandResult(resultObj));
        }
    }

    void setUp() override {
        ShardServerTestFixture::setUp();

        for (const auto& shardId : kShardIdList) {
            auto shardTargeter = RemoteCommandTargeterMock::get(
                uassertStatusOK(shardRegistry()->getShard(operationContext(), shardId))
                    ->getTargeter());
            shardTargeter->setConnectionStringReturnValue(
                ConnectionString::forReplicaSet(shardId.toString(), {makeHostAndPort(shardId)}));
        }
    }

    std::unique_ptr<ShardingCatalogClient> makeShardingCatalogClient(
        std::unique_ptr<DistLockManager> distLockManager) override {

        class StaticCatalogClient final : public ShardingCatalogClientMock {
        public:
            StaticCatalogClient(std::vector<ShardId> shardIds)
                : ShardingCatalogClientMock(nullptr), _shardIds(std::move(shardIds)) {}

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
};

//
// No errors
//

TEST_F(AppendRawResponsesTest, AllShardsReturnSuccess) {
    std::vector<AsyncRequestsSender::Response> shardResponses{
        {kShard1, kOkResponse, makeHostAndPort(kShard1)},
        {kShard2, kOkResponse, makeHostAndPort(kShard2)},
        {kShard3, kOkResponse, makeHostAndPort(kShard3)}};

    runAppendRawResponsesExpect(shardResponses, Status::OK(), {kShard1, kShard2, kShard3});
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

    runAppendRawResponsesExpect(
        shardResponses, Status::OK(), {kShard1, kShard2, kShard3}, kWriteConcernError1Status);
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
    runAppendRawResponsesExpect(
        shardResponses, Status::OK(), {kShard1, kShard2, kShard3}, kWriteConcernError1Status);
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
    runAppendRawResponsesExpect(
        shardResponses, Status::OK(), {kShard1, kShard2, kShard3}, kWriteConcernError1Status);
}

//
// Errors *sending* the requests.
//

TEST_F(AppendRawResponsesTest, AllAttemptsToSendRequestsReturnSameIgnorableError) {
    std::vector<AsyncRequestsSender::Response> shardResponses{
        {kShard1, kIgnorableError1Status, boost::none},
        {kShard2, kIgnorableError1Status, boost::none},
        {kShard3, kIgnorableError1Status, boost::none}};

    // The ignorable error gets promoted to a non-ignorable error.
    runAppendRawResponsesExpect(
        shardResponses, kIgnorableError1Status, {kShard1, kShard2, kShard3});
}

TEST_F(AppendRawResponsesTest,
       SomeShardsReturnSuccessRestOfAttemptsToSendRequestsReturnSameIgnorableError) {
    std::vector<AsyncRequestsSender::Response> shardResponses{
        {kShard1, kOkResponse, makeHostAndPort(kShard1)},
        {kShard2, kOkResponse, makeHostAndPort(kShard2)},
        {kShard3, kIgnorableError1Status, boost::none},
        {kShard4, kIgnorableError1Status, boost::none}};

    // The ignorable errors are ignored.
    runAppendRawResponsesExpect(shardResponses, Status::OK(), {kShard1, kShard2});
}

TEST_F(AppendRawResponsesTest, AttemptsToSendRequestsReturnDifferentIgnorableErrors) {
    std::vector<AsyncRequestsSender::Response> shardResponses{
        {kShard1, kIgnorableError1Status, boost::none},
        {kShard2, kIgnorableError2Status, boost::none},
        {kShard3, kIgnorableError1Status, boost::none}};

    // The *first* ignorable error gets promoted to a non-ignorable error.
    runAppendRawResponsesExpect(
        shardResponses, kIgnorableError1Status, {kShard1, kShard2, kShard3});
}

TEST_F(AppendRawResponsesTest,
       SomeShardsReturnSuccessRestOfAttemptsToSendRequestsReturnDifferentIgnorableErrors) {
    std::vector<AsyncRequestsSender::Response> shardResponses{
        {kShard1, kOkResponse, makeHostAndPort(kShard1)},
        {kShard2, kOkResponse, makeHostAndPort(kShard2)},
        {kShard3, kIgnorableError1Status, boost::none},
        {kShard4, kIgnorableError2Status, boost::none}};

    // The ignorable errors are ignored.
    runAppendRawResponsesExpect(shardResponses, Status::OK(), {kShard1, kShard2});
}

TEST_F(AppendRawResponsesTest, AllAttemptsToSendRequestsReturnSameNonIgnorableError) {
    std::vector<AsyncRequestsSender::Response> shardResponses{
        {kShard1, kNonIgnorableError1Status, boost::none},
        {kShard2, kNonIgnorableError1Status, boost::none},
        {kShard3, kNonIgnorableError1Status, boost::none}};

    // The non-ignorable error is returned.
    runAppendRawResponsesExpect(
        shardResponses, kNonIgnorableError1Status, {kShard1, kShard2, kShard3});
}

TEST_F(AppendRawResponsesTest,
       SomeShardsReturnSuccessRestOfAttemptsToSendRequestsReturnSameNonIgnorableError) {
    std::vector<AsyncRequestsSender::Response> shardResponses{
        {kShard1, kOkResponse, makeHostAndPort(kShard1)},
        {kShard2, kOkResponse, makeHostAndPort(kShard2)},
        {kShard3, kNonIgnorableError1Status, boost::none},
        {kShard4, kNonIgnorableError1Status, boost::none}};

    // The non-ignorable error is returned.
    runAppendRawResponsesExpect(
        shardResponses, kNonIgnorableError1Status, {kShard1, kShard2, kShard3, kShard4});
}

TEST_F(AppendRawResponsesTest, AttemptsToSendRequestsReturnDifferentNonIgnorableErrors) {
    std::vector<AsyncRequestsSender::Response> shardResponses{
        {kShard1, kNonIgnorableError1Status, boost::none},
        {kShard2, kNonIgnorableError2Status, boost::none},
        {kShard3, kNonIgnorableError1Status, boost::none}};

    // The first non-ignorable error is returned.
    runAppendRawResponsesExpect(
        shardResponses, kNonIgnorableError1Status, {kShard1, kShard2, kShard3});
}

TEST_F(AppendRawResponsesTest,
       SomeShardsReturnSuccessRestOfAttemptsToSendRequestsReturnDifferentNonIgnorableErrors) {
    std::vector<AsyncRequestsSender::Response> shardResponses{
        {kShard1, kOkResponse, makeHostAndPort(kShard1)},
        {kShard2, kOkResponse, makeHostAndPort(kShard2)},
        {kShard3, kNonIgnorableError1Status, boost::none},
        {kShard4, kNonIgnorableError2Status, boost::none}};

    // The first non-ignorable error is returned.
    runAppendRawResponsesExpect(
        shardResponses, kNonIgnorableError1Status, {kShard1, kShard2, kShard3, kShard4});
}

TEST_F(AppendRawResponsesTest, AllAttemptsToSendRequestsReturnErrorsSomeIgnorableSomeNonIgnorable) {
    std::vector<AsyncRequestsSender::Response> shardResponses{
        {kShard1, kIgnorableError1Status, boost::none},
        {kShard2, kIgnorableError2Status, boost::none},
        {kShard3, kNonIgnorableError1Status, boost::none},
        {kShard4, kNonIgnorableError2Status, boost::none}};

    // The first non-ignorable error is returned.
    runAppendRawResponsesExpect(shardResponses, kNonIgnorableError1Status, {kShard3, kShard4});
}

TEST_F(
    AppendRawResponsesTest,
    SomeShardsReturnSuccessRestOfAttemptsToSendRequestsReturnErrorsSomeIgnorableSomeNonIgnorable) {
    std::vector<AsyncRequestsSender::Response> shardResponses{
        {kShard1, kIgnorableError1Status, boost::none},
        {kShard2, kOkResponse, makeHostAndPort(kShard2)},
        {kShard3, kNonIgnorableError1Status, boost::none},
        {kShard4, kNonIgnorableError2Status, boost::none}};

    // The first non-ignorable error is returned.
    runAppendRawResponsesExpect(
        shardResponses, kNonIgnorableError1Status, {kShard2, kShard3, kShard4});
}

//
// Errors *processing* the requests.
//

TEST_F(AppendRawResponsesTest, AllShardsReturnSameIgnorableError) {
    std::vector<AsyncRequestsSender::Response> shardResponses{
        {kShard1, makeErrorResponse(kIgnorableError1Status), makeHostAndPort(kShard1)},
        {kShard2, makeErrorResponse(kIgnorableError1Status), makeHostAndPort(kShard2)},
        {kShard3, makeErrorResponse(kIgnorableError1Status), makeHostAndPort(kShard3)}};

    // The ignorable error gets promoted to a non-ignorable error.
    runAppendRawResponsesExpect(
        shardResponses, kIgnorableError1Status, {kShard1, kShard2, kShard3});
}

TEST_F(AppendRawResponsesTest, SomeShardsReturnSuccessRestReturnSameIgnorableError) {
    std::vector<AsyncRequestsSender::Response> shardResponses{
        {kShard1, kOkResponse, makeHostAndPort(kShard1)},
        {kShard2, kOkResponse, makeHostAndPort(kShard2)},
        {kShard3, makeErrorResponse(kIgnorableError1Status), makeHostAndPort(kShard3)},
        {kShard4, makeErrorResponse(kIgnorableError1Status), makeHostAndPort(kShard4)}};

    // The ignorable errors are ignored.
    runAppendRawResponsesExpect(shardResponses, Status::OK(), {kShard1, kShard2});
}

TEST_F(AppendRawResponsesTest, ShardsReturnDifferentIgnorableErrors) {
    std::vector<AsyncRequestsSender::Response> shardResponses{
        {kShard1, makeErrorResponse(kIgnorableError1Status), makeHostAndPort(kShard1)},
        {kShard2, makeErrorResponse(kIgnorableError2Status), makeHostAndPort(kShard2)},
        {kShard3, makeErrorResponse(kIgnorableError1Status), makeHostAndPort(kShard3)}};

    // The *first* ignorable error gets promoted to a non-ignorable error.
    runAppendRawResponsesExpect(
        shardResponses, kIgnorableError1Status, {kShard1, kShard2, kShard3});
}

TEST_F(AppendRawResponsesTest, SomeShardsReturnSuccessRestReturnDifferentIgnorableErrors) {
    std::vector<AsyncRequestsSender::Response> shardResponses{
        {kShard1, kOkResponse, makeHostAndPort(kShard1)},
        {kShard2, kOkResponse, makeHostAndPort(kShard2)},
        {kShard3, makeErrorResponse(kIgnorableError1Status), makeHostAndPort(kShard3)},
        {kShard4, makeErrorResponse(kIgnorableError2Status), makeHostAndPort(kShard4)}};

    // The ignorable errors are ignored.
    runAppendRawResponsesExpect(shardResponses, Status::OK(), {kShard1, kShard2});
}

TEST_F(AppendRawResponsesTest, AllShardsReturnSameNonIgnorableError) {
    std::vector<AsyncRequestsSender::Response> shardResponses{
        {kShard1, makeErrorResponse(kNonIgnorableError1Status), makeHostAndPort(kShard1)},
        {kShard2, makeErrorResponse(kNonIgnorableError1Status), makeHostAndPort(kShard2)},
        {kShard3, makeErrorResponse(kNonIgnorableError1Status), makeHostAndPort(kShard3)}};

    // The non-ignorable error is returned.
    runAppendRawResponsesExpect(
        shardResponses, kNonIgnorableError1Status, {kShard1, kShard2, kShard3});
}

TEST_F(AppendRawResponsesTest, SomeShardsReturnSuccessRestReturnSameNonIgnorableError) {
    std::vector<AsyncRequestsSender::Response> shardResponses{
        {kShard1, kOkResponse, makeHostAndPort(kShard1)},
        {kShard2, kOkResponse, makeHostAndPort(kShard2)},
        {kShard3, makeErrorResponse(kNonIgnorableError1Status), makeHostAndPort(kShard3)},
        {kShard4, makeErrorResponse(kNonIgnorableError1Status), makeHostAndPort(kShard4)}};

    // The non-ignorable error is returned.
    runAppendRawResponsesExpect(
        shardResponses, kNonIgnorableError1Status, {kShard1, kShard2, kShard3, kShard4});
}

TEST_F(AppendRawResponsesTest, ShardsReturnDifferentNonIgnorableErrors) {
    std::vector<AsyncRequestsSender::Response> shardResponses{
        {kShard1, makeErrorResponse(kNonIgnorableError1Status), makeHostAndPort(kShard1)},
        {kShard2, makeErrorResponse(kNonIgnorableError2Status), makeHostAndPort(kShard2)},
        {kShard3, makeErrorResponse(kNonIgnorableError1Status), makeHostAndPort(kShard3)}};

    // The first non-ignorable error is returned.
    runAppendRawResponsesExpect(
        shardResponses, kNonIgnorableError1Status, {kShard1, kShard2, kShard3});
}

TEST_F(AppendRawResponsesTest, SomeShardsReturnSuccessRestReturnDifferentNonIgnorableErrors) {
    std::vector<AsyncRequestsSender::Response> shardResponses{
        {kShard1, kOkResponse, makeHostAndPort(kShard1)},
        {kShard2, kOkResponse, makeHostAndPort(kShard2)},
        {kShard3, makeErrorResponse(kNonIgnorableError1Status), makeHostAndPort(kShard3)},
        {kShard4, makeErrorResponse(kNonIgnorableError2Status), makeHostAndPort(kShard4)}};

    // The first non-ignorable error is returned.
    runAppendRawResponsesExpect(
        shardResponses, kNonIgnorableError1Status, {kShard1, kShard2, kShard3, kShard4});
}

TEST_F(AppendRawResponsesTest, AllShardsReturnErrorsSomeIgnorableSomeNonIgnorable) {
    std::vector<AsyncRequestsSender::Response> shardResponses{
        {kShard1, makeErrorResponse(kIgnorableError1Status), makeHostAndPort(kShard1)},
        {kShard2, makeErrorResponse(kIgnorableError2Status), makeHostAndPort(kShard2)},
        {kShard3, makeErrorResponse(kNonIgnorableError1Status), makeHostAndPort(kShard3)},
        {kShard4, makeErrorResponse(kNonIgnorableError2Status), makeHostAndPort(kShard4)}};

    // The first non-ignorable error is returned.
    runAppendRawResponsesExpect(shardResponses, kNonIgnorableError1Status, {kShard3, kShard4});
}

TEST_F(AppendRawResponsesTest,
       SomeShardsReturnSuccessRestReturnErrorsSomeIgnorableSomeNonIgnorable) {
    std::vector<AsyncRequestsSender::Response> shardResponses{
        {kShard1, makeErrorResponse(kIgnorableError1Status), makeHostAndPort(kShard1)},
        {kShard2, kOkResponse, makeHostAndPort(kShard2)},
        {kShard3, makeErrorResponse(kNonIgnorableError1Status), makeHostAndPort(kShard3)},
        {kShard4, makeErrorResponse(kNonIgnorableError2Status), makeHostAndPort(kShard4)}};

    // The first non-ignorable error is returned.
    runAppendRawResponsesExpect(
        shardResponses, kNonIgnorableError1Status, {kShard2, kShard3, kShard4});
}

// Mix of errors *sending* and *processing* the requests.

TEST_F(AppendRawResponsesTest,
       AllShardsReturnErrorsMixOfErrorsSendingRequestsAndErrorsProcessingRequests) {
    std::vector<AsyncRequestsSender::Response> shardResponses{
        {kShard1, kIgnorableError1Status, boost::none},
        {kShard2, makeErrorResponse(kIgnorableError2Status), makeHostAndPort(kShard2)},
        {kShard3, kNonIgnorableError1Status, boost::none},
        {kShard4, makeErrorResponse(kNonIgnorableError2Status), makeHostAndPort(kShard4)}};

    // The first non-ignorable error is returned.
    runAppendRawResponsesExpect(shardResponses, kNonIgnorableError1Status, {kShard3, kShard4});
}

TEST_F(
    AppendRawResponsesTest,
    SomeShardsReturnSuccessRestReturnErrorsMixOfErrorsSendingRequestsAndErrorsProcessingRequests) {
    std::vector<AsyncRequestsSender::Response> shardResponses{
        {kShard1, kIgnorableError1Status, boost::none},
        {kShard2, makeErrorResponse(kIgnorableError2Status), makeHostAndPort(kShard2)},
        {kShard3, kNonIgnorableError1Status, boost::none},
        {kShard4, makeErrorResponse(kNonIgnorableError2Status), makeHostAndPort(kShard4)},
        {kShard5, kOkResponse, HostAndPort("e:1")}};

    // The first non-ignorable error is returned.
    runAppendRawResponsesExpect(
        shardResponses, kNonIgnorableError1Status, {kShard3, kShard4, kShard5});
}

// Mix of errors sending or processing the requests *and* writeConcern error.

TEST_F(AppendRawResponsesTest, SomeShardsReturnSuccessWithWriteConcernErrorRestReturnMixOfErrors) {
    std::vector<AsyncRequestsSender::Response> shardResponses{
        {kShard1, kIgnorableError1Status, boost::none},
        {kShard2, makeErrorResponse(kIgnorableError2Status), makeHostAndPort(kShard2)},
        {kShard3, kNonIgnorableError1Status, boost::none},
        {kShard4, makeErrorResponse(kNonIgnorableError2Status), makeHostAndPort(kShard4)},
        {kShard5,
         makeWriteConcernErrorResponse(kWriteConcernError1Status),
         makeHostAndPort(kShard5)}};

    // The first non-ignorable error is returned, and no writeConcern error is reported at the top
    // level.
    runAppendRawResponsesExpect(
        shardResponses, kNonIgnorableError1Status, {kShard3, kShard4, kShard5});
}

}  // namespace mongo
