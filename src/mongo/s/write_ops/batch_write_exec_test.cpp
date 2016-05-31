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

#include "mongo/base/owned_pointer_vector.h"
#include "mongo/client/remote_command_targeter_factory_mock.h"
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/client/mock_multi_write_command.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/mock_ns_targeter.h"
#include "mongo/s/sharding_test_fixture.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"
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

        // Make the batch write executor use the mock backend.
        exec.reset(new BatchWriteExec(&nsTargeter, &dispatcher));
    }

    void setMockResults(const vector<MockWriteResult*>& results) {
        dispatcher.init(results);
    }

    ConnectionString shardHost{kTestShardHost};
    NamespaceString nss{"foo.bar"};

    MockNSTargeter nsTargeter;
    MockMultiWriteCommand dispatcher;

    unique_ptr<BatchWriteExec> exec;
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
    request.getInsertRequest()->addToDocuments(BSON("x" << 1));

    BatchedCommandResponse response;
    BatchWriteExecStats stats;
    exec->executeBatch(operationContext(), request, &response, &stats);
    ASSERT(response.getOk());

    ASSERT_EQUALS(stats.numRounds, 1);
}

TEST_F(BatchWriteExecTest, SingleOpError) {
    //
    // Basic error test
    //

    vector<MockWriteResult*> mockResults;
    BatchedCommandResponse errResponse;
    errResponse.setOk(false);
    errResponse.setErrCode(ErrorCodes::UnknownError);
    errResponse.setErrMessage("mock error");
    mockResults.push_back(new MockWriteResult(shardHost, errResponse));

    setMockResults(mockResults);

    BatchedCommandRequest request(BatchedCommandRequest::BatchType_Insert);
    request.setNS(nss);
    request.setOrdered(false);
    request.setWriteConcern(BSONObj());
    // Do single-target, single doc batch write op
    request.getInsertRequest()->addToDocuments(BSON("x" << 1));

    BatchedCommandResponse response;
    BatchWriteExecStats stats;
    exec->executeBatch(operationContext(), request, &response, &stats);
    ASSERT(response.getOk());
    ASSERT_EQUALS(response.getN(), 0);
    ASSERT(response.isErrDetailsSet());
    ASSERT_EQUALS(response.getErrDetailsAt(0)->getErrCode(), errResponse.getErrCode());
    ASSERT(response.getErrDetailsAt(0)->getErrMessage().find(errResponse.getErrMessage()) !=
           string::npos);

    ASSERT_EQUALS(stats.numRounds, 1);
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
    request.getInsertRequest()->addToDocuments(BSON("x" << 1));

    vector<MockWriteResult*> mockResults;
    WriteErrorDetail error;
    error.setErrCode(ErrorCodes::StaleShardVersion);
    error.setErrMessage("mock stale error");
    mockResults.push_back(new MockWriteResult(shardHost, error));

    setMockResults(mockResults);

    // Execute request
    BatchedCommandResponse response;
    BatchWriteExecStats stats;
    exec->executeBatch(operationContext(), request, &response, &stats);
    ASSERT(response.getOk());

    ASSERT_EQUALS(stats.numStaleBatches, 1);
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
    request.getInsertRequest()->addToDocuments(BSON("x" << 1));

    vector<MockWriteResult*> mockResults;
    WriteErrorDetail error;
    error.setErrCode(ErrorCodes::StaleShardVersion);
    error.setErrMessage("mock stale error");
    for (int i = 0; i < 3; i++) {
        mockResults.push_back(new MockWriteResult(shardHost, error));
    }

    setMockResults(mockResults);

    // Execute request
    BatchedCommandResponse response;
    BatchWriteExecStats stats;
    exec->executeBatch(operationContext(), request, &response, &stats);
    ASSERT(response.getOk());

    ASSERT_EQUALS(stats.numStaleBatches, 3);
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
    request.getInsertRequest()->addToDocuments(BSON("x" << 1));
    request.getInsertRequest()->addToDocuments(BSON("x" << 2));

    vector<MockWriteResult*> mockResults;
    WriteErrorDetail error;
    error.setErrCode(ErrorCodes::StaleShardVersion);
    error.setErrMessage("mock stale error");
    for (int i = 0; i < 10; i++) {
        mockResults.push_back(new MockWriteResult(shardHost, error, request.sizeWriteOps()));
    }

    setMockResults(mockResults);

    // Execute request
    BatchedCommandResponse response;
    BatchWriteExecStats stats;
    exec->executeBatch(operationContext(), request, &response, &stats);
    ASSERT(response.getOk());
    ASSERT_EQUALS(response.getN(), 0);
    ASSERT(response.isErrDetailsSet());
    ASSERT_EQUALS(response.getErrDetailsAt(0)->getErrCode(), ErrorCodes::NoProgressMade);
    ASSERT_EQUALS(response.getErrDetailsAt(1)->getErrCode(), ErrorCodes::NoProgressMade);
}

TEST_F(BatchWriteExecTest, ManyStaleOpWithMigration) {
    //
    // Retry op in exec many times b/c of stale config, but simulate remote migrations occurring
    //

    // Insert request
    BatchedCommandRequest request(BatchedCommandRequest::BatchType_Insert);
    request.setNS(nss);
    request.setOrdered(false);
    request.setWriteConcern(BSONObj());
    // Do single-target, single doc batch write op
    request.getInsertRequest()->addToDocuments(BSON("x" << 1));

    vector<MockWriteResult*> mockResults;
    WriteErrorDetail error;
    error.setErrCode(ErrorCodes::StaleShardVersion);
    error.setErrMessage("mock stale error");
    for (int i = 0; i < 10; i++) {
        mockResults.push_back(new MockWriteResult(shardHost, error));
    }

    setMockResults(mockResults);

    // Execute request
    BatchedCommandResponse response;
    BatchWriteExecStats stats;
    exec->executeBatch(operationContext(), request, &response, &stats);
    ASSERT(response.getOk());

    ASSERT_EQUALS(stats.numStaleBatches, 6);
}

}  // namespace
}  // namespace mongo
