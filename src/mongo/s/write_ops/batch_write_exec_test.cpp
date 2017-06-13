/**
 *    Copyright (C) 2013 MongoDB Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/s/write_ops/batch_write_exec.h"

#include "mongo/client/remote_command_targeter_factory_mock.h"
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/sharding_test_fixture.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/s/write_ops/mock_ns_targeter.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

using std::unique_ptr;
using std::string;
using std::vector;

namespace {

const HostAndPort kTestShardHost = HostAndPort("FakeHost", 12345);
const HostAndPort kTestConfigShardHost = HostAndPort("FakeConfigHost", 12345);
const string shardName = "FakeShard";
const int kMaxRoundsWithoutProgress = 5;

/**
 * Mimics a single shard backend for a particular collection which can be initialized with a
 * set of write command results to return.
 */
class BatchWriteExecTest : public ShardingTestFixture {
public:
    BatchWriteExecTest() = default;
    ~BatchWriteExecTest() = default;

    void setUp() override {
        ShardingTestFixture::setUp();
        setRemote(HostAndPort("ClientHost", 12345));

        // Set up the RemoteCommandTargeter for the config shard.
        configTargeter()->setFindHostReturnValue(kTestConfigShardHost);

        // Add a RemoteCommandTargeter for the data shard.
        std::unique_ptr<RemoteCommandTargeterMock> targeter(
            stdx::make_unique<RemoteCommandTargeterMock>());
        targeter->setConnectionStringReturnValue(ConnectionString(kTestShardHost));
        targeter->setFindHostReturnValue(kTestShardHost);
        targeterFactory()->addTargeterToReturn(ConnectionString(kTestShardHost),
                                               std::move(targeter));

        // Set up the shard registry to contain the fake shard.
        ShardType shardType;
        shardType.setName(shardName);
        shardType.setHost(kTestShardHost.toString());
        std::vector<ShardType> shards{shardType};
        setupShards(shards);

        // Set up the namespace targeter to target the fake shard.
        ShardEndpoint endpoint(shardName, ChunkVersion::IGNORED());
        vector<MockRange*> mockRanges;
        mockRanges.push_back(
            new MockRange(endpoint, nss, BSON("x" << MINKEY), BSON("x" << MAXKEY)));
        nsTargeter.init(mockRanges);
    }

    void expectInsertsReturnSuccess(const std::vector<BSONObj>& expected) {
        onCommandForPoolExecutor([&](const executor::RemoteCommandRequest& request) {
            ASSERT_EQUALS(nss.db(), request.dbname);

            BatchedInsertRequest actualBatchedInsert;
            actualBatchedInsert.parseRequest(
                OpMsgRequest::fromDBAndBody(request.dbname, request.cmdObj));

            ASSERT_EQUALS(nss.toString(), actualBatchedInsert.getNS().toString());

            auto inserted = actualBatchedInsert.getDocuments();
            ASSERT_EQUALS(expected.size(), inserted.size());

            auto itInserted = inserted.begin();
            auto itExpected = expected.begin();

            for (; itInserted != inserted.end(); itInserted++, itExpected++) {
                ASSERT_BSONOBJ_EQ(*itExpected, *itInserted);
            }

            BatchedCommandResponse response;
            response.setOk(true);

            return response.toBSON();
        });
    }

    void expectInsertsReturnStaleVersionErrors(const std::vector<BSONObj>& expected) {
        WriteErrorDetail error;
        error.setErrCode(ErrorCodes::StaleShardVersion);
        error.setErrMessage("mock stale error");
        onCommandForPoolExecutor([&](const executor::RemoteCommandRequest& request) {
            ASSERT_EQUALS(nss.db(), request.dbname);

            BatchedInsertRequest actualBatchedInsert;
            actualBatchedInsert.parseRequest(
                OpMsgRequest::fromDBAndBody(request.dbname, request.cmdObj));

            ASSERT_EQUALS(nss.toString(), actualBatchedInsert.getNS().toString());

            auto inserted = actualBatchedInsert.getDocuments();
            ASSERT_EQUALS(expected.size(), inserted.size());

            auto itInserted = inserted.begin();
            auto itExpected = expected.begin();

            for (; itInserted != inserted.end(); itInserted++, itExpected++) {
                ASSERT_BSONOBJ_EQ(*itExpected, *itInserted);
            }

            BatchedCommandResponse staleResponse;
            staleResponse.setOk(true);
            staleResponse.setN(0);

            // Report a stale version error for each write in the batch.
            int i = 0;
            for (itInserted = inserted.begin(); itInserted != inserted.end(); ++itInserted) {
                WriteErrorDetail* errorCopy = new WriteErrorDetail;
                error.cloneTo(errorCopy);
                errorCopy->setIndex(i);
                staleResponse.addToErrDetails(errorCopy);
                ++i;
            }

            return staleResponse.toBSON();
        });
    }

    ConnectionString shardHost{kTestShardHost};
    NamespaceString nss{"foo.bar"};

    MockNSTargeter nsTargeter;
};

//
// Tests for the BatchWriteExec
//

TEST_F(BatchWriteExecTest, SingleOp) {
    //
    // Basic execution test
    //

    BatchedCommandRequest request(BatchedCommandRequest::BatchType_Insert);
    request.setNS(nss);
    request.setOrdered(false);
    request.setWriteConcern(BSONObj());
    // Do single-target, single doc batch write op
    auto objToInsert = BSON("x" << 1);
    request.getInsertRequest()->addToDocuments(objToInsert);

    auto future = launchAsync([&] {
        BatchedCommandResponse response;
        BatchWriteExecStats stats;
        BatchWriteExec::executeBatch(operationContext(), nsTargeter, request, &response, &stats);
        ASSERT(response.getOk());
        ASSERT_EQUALS(stats.numRounds, 1);
    });

    std::vector<BSONObj> expected{objToInsert};
    expectInsertsReturnSuccess(expected);

    future.timed_get(kFutureTimeout);
}

TEST_F(BatchWriteExecTest, SingleOpError) {
    //
    // Basic error test
    //

    BatchedCommandResponse errResponse;
    errResponse.setOk(false);
    errResponse.setErrCode(ErrorCodes::UnknownError);
    errResponse.setErrMessage("mock error");

    BatchedCommandRequest request(BatchedCommandRequest::BatchType_Insert);
    request.setNS(nss);
    request.setOrdered(false);
    request.setWriteConcern(BSONObj());
    // Do single-target, single doc batch write op
    auto objToInsert = BSON("x" << 1);
    request.getInsertRequest()->addToDocuments(objToInsert);

    auto future = launchAsync([&] {
        BatchedCommandResponse response;
        BatchWriteExecStats stats;
        BatchWriteExec::executeBatch(operationContext(), nsTargeter, request, &response, &stats);
        ASSERT(response.getOk());
        ASSERT_EQUALS(response.getN(), 0);
        ASSERT(response.isErrDetailsSet());
        ASSERT_EQUALS(response.getErrDetailsAt(0)->getErrCode(), errResponse.getErrCode());
        ASSERT(response.getErrDetailsAt(0)->getErrMessage().find(errResponse.getErrMessage()) !=
               string::npos);

        ASSERT_EQUALS(stats.numRounds, 1);
    });

    std::vector<BSONObj> expected{objToInsert};
    onCommandForPoolExecutor([&](const executor::RemoteCommandRequest& request) {
        ASSERT_EQUALS(nss.db(), request.dbname);

        BatchedInsertRequest actualBatchedInsert;
        actualBatchedInsert.parseRequest(
            OpMsgRequest::fromDBAndBody(request.dbname, request.cmdObj));

        ASSERT_EQUALS(nss.toString(), actualBatchedInsert.getNS().toString());

        auto inserted = actualBatchedInsert.getDocuments();
        ASSERT_EQUALS(expected.size(), inserted.size());

        auto itInserted = inserted.begin();
        auto itExpected = expected.begin();

        for (; itInserted != inserted.end(); itInserted++, itExpected++) {
            ASSERT_BSONOBJ_EQ(*itExpected, *itInserted);
        }

        return errResponse.toBSON();
    });

    future.timed_get(kFutureTimeout);
}

//
// Test retryable errors
//

TEST_F(BatchWriteExecTest, StaleOp) {
    //
    // Retry op in exec b/c of stale config
    //

    // Insert request
    BatchedCommandRequest request(BatchedCommandRequest::BatchType_Insert);
    request.setNS(nss);
    request.setOrdered(false);
    request.setWriteConcern(BSONObj());
    // Do single-target, single doc batch write op
    auto objToInsert = BSON("x" << 1);
    request.getInsertRequest()->addToDocuments(objToInsert);

    // Execute request
    auto future = launchAsync([&] {
        BatchedCommandResponse response;
        BatchWriteExecStats stats;
        BatchWriteExec::executeBatch(operationContext(), nsTargeter, request, &response, &stats);
        ASSERT(response.getOk());

        ASSERT_EQUALS(stats.numStaleBatches, 1);
    });

    std::vector<BSONObj> expected{objToInsert};
    expectInsertsReturnStaleVersionErrors(expected);
    expectInsertsReturnSuccess(expected);

    future.timed_get(kFutureTimeout);
}

TEST_F(BatchWriteExecTest, MultiStaleOp) {
    //
    // Retry op in exec multiple times b/c of stale config
    //

    // Insert request
    BatchedCommandRequest request(BatchedCommandRequest::BatchType_Insert);
    request.setNS(nss);
    request.setOrdered(false);
    request.setWriteConcern(BSONObj());
    // Do single-target, single doc batch write op
    auto objToInsert = BSON("x" << 1);
    request.getInsertRequest()->addToDocuments(objToInsert);

    auto future = launchAsync([&] {
        BatchedCommandResponse response;
        BatchWriteExecStats stats;
        BatchWriteExec::executeBatch(operationContext(), nsTargeter, request, &response, &stats);
        ASSERT(response.getOk());

        ASSERT_EQUALS(stats.numStaleBatches, 3);
    });

    std::vector<BSONObj> expected{objToInsert};

    // Return multiple StaleShardVersion errors
    for (int i = 0; i < 3; i++) {
        expectInsertsReturnStaleVersionErrors(expected);
    }

    expectInsertsReturnSuccess(expected);

    future.timed_get(kFutureTimeout);
}

TEST_F(BatchWriteExecTest, TooManyStaleOp) {
    //
    // Retry op in exec too many times (without refresh) b/c of stale config
    // (The mock nsTargeter doesn't report progress on refresh)
    // We should report a no progress error for everything in the batch
    //

    // Insert request
    BatchedCommandRequest request(BatchedCommandRequest::BatchType_Insert);
    request.setNS(nss);
    request.setOrdered(false);
    request.setWriteConcern(BSONObj());
    // Do single-target, single doc batch write ops
    auto objToInsert1 = BSON("x" << 1);
    auto objToInsert2 = BSON("x" << 2);
    request.getInsertRequest()->addToDocuments(objToInsert1);
    request.getInsertRequest()->addToDocuments(objToInsert2);

    auto future = launchAsync([&] {
        BatchedCommandResponse response;
        BatchWriteExecStats stats;
        BatchWriteExec::executeBatch(operationContext(), nsTargeter, request, &response, &stats);
        ASSERT(response.getOk());
        ASSERT_EQUALS(response.getN(), 0);
        ASSERT(response.isErrDetailsSet());
        ASSERT_EQUALS(response.getErrDetailsAt(0)->getErrCode(), ErrorCodes::NoProgressMade);
        ASSERT_EQUALS(response.getErrDetailsAt(1)->getErrCode(), ErrorCodes::NoProgressMade);

        ASSERT_EQUALS(stats.numStaleBatches, (1 + kMaxRoundsWithoutProgress));
    });

    std::vector<BSONObj> expected{objToInsert1, objToInsert2};

    // Return multiple StaleShardVersion errors
    for (int i = 0; i < (1 + kMaxRoundsWithoutProgress); i++) {
        expectInsertsReturnStaleVersionErrors(expected);
    }

    future.timed_get(kFutureTimeout);
}

}  // namespace
}  // namespace mongo
