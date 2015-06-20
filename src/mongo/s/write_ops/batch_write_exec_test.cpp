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

#include "mongo/s/write_ops/batch_write_exec.h"


#include "mongo/base/owned_pointer_vector.h"
#include "mongo/s/client/mock_multi_write_command.h"
#include "mongo/s/mock_ns_targeter.h"
#include "mongo/s/mock_shard_resolver.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/unittest/unittest.h"

namespace {

using std::unique_ptr;
using std::string;
using std::vector;

using namespace mongo;

/**
 * Mimics a single shard backend for a particular collection which can be initialized with a
 * set of write command results to return.
 */
class MockSingleShardBackend {
public:
    MockSingleShardBackend(const NamespaceString& nss) {
        // Initialize targeting to a mock shard
        ShardEndpoint endpoint("shard", ChunkVersion::IGNORED());
        vector<MockRange*> mockRanges;
        mockRanges.push_back(
            new MockRange(endpoint, nss, BSON("x" << MINKEY), BSON("x" << MAXKEY)));
        targeter.init(mockRanges);

        // Get the connection string for the mock shard
        resolver.chooseWriteHost(mockRanges.front()->endpoint.shardName, &shardHost);

        // Executor using the mock backend
        exec.reset(new BatchWriteExec(&targeter, &resolver, &dispatcher));
    }

    void setMockResults(const vector<MockWriteResult*>& results) {
        dispatcher.init(results);
    }

    ConnectionString shardHost;

    MockNSTargeter targeter;
    MockShardResolver resolver;
    MockMultiWriteCommand dispatcher;

    unique_ptr<BatchWriteExec> exec;
};

//
// Tests for the BatchWriteExec
//

TEST(BatchWriteExecTests, SingleOp) {
    //
    // Basic execution test
    //

    NamespaceString nss("foo.bar");

    MockSingleShardBackend backend(nss);

    BatchedCommandRequest request(BatchedCommandRequest::BatchType_Insert);
    request.setNS(nss.ns());
    request.setOrdered(false);
    request.setWriteConcern(BSONObj());
    // Do single-target, single doc batch write op
    request.getInsertRequest()->addToDocuments(BSON("x" << 1));

    BatchedCommandResponse response;
    backend.exec->executeBatch(request, &response);
    ASSERT(response.getOk());

    const BatchWriteExecStats& stats = backend.exec->getStats();
    ASSERT_EQUALS(stats.numRounds, 1);
}

TEST(BatchWriteExecTests, SingleOpError) {
    //
    // Basic error test
    //

    NamespaceString nss("foo.bar");

    MockSingleShardBackend backend(nss);

    vector<MockWriteResult*> mockResults;
    BatchedCommandResponse errResponse;
    errResponse.setOk(false);
    errResponse.setErrCode(ErrorCodes::UnknownError);
    errResponse.setErrMessage("mock error");
    mockResults.push_back(new MockWriteResult(backend.shardHost, errResponse));

    backend.setMockResults(mockResults);

    BatchedCommandRequest request(BatchedCommandRequest::BatchType_Insert);
    request.setNS(nss.ns());
    request.setOrdered(false);
    request.setWriteConcern(BSONObj());
    // Do single-target, single doc batch write op
    request.getInsertRequest()->addToDocuments(BSON("x" << 1));

    BatchedCommandResponse response;
    backend.exec->executeBatch(request, &response);
    ASSERT(response.getOk());
    ASSERT_EQUALS(response.getN(), 0);
    ASSERT(response.isErrDetailsSet());
    ASSERT_EQUALS(response.getErrDetailsAt(0)->getErrCode(), errResponse.getErrCode());
    ASSERT(response.getErrDetailsAt(0)->getErrMessage().find(errResponse.getErrMessage()) !=
           string::npos);

    const BatchWriteExecStats& stats = backend.exec->getStats();
    ASSERT_EQUALS(stats.numRounds, 1);
}

//
// Test retryable errors
//

TEST(BatchWriteExecTests, StaleOp) {
    //
    // Retry op in exec b/c of stale config
    //

    NamespaceString nss("foo.bar");

    // Insert request
    BatchedCommandRequest request(BatchedCommandRequest::BatchType_Insert);
    request.setNS(nss.ns());
    request.setOrdered(false);
    request.setWriteConcern(BSONObj());
    // Do single-target, single doc batch write op
    request.getInsertRequest()->addToDocuments(BSON("x" << 1));

    MockSingleShardBackend backend(nss);

    vector<MockWriteResult*> mockResults;
    WriteErrorDetail error;
    error.setErrCode(ErrorCodes::StaleShardVersion);
    error.setErrMessage("mock stale error");
    mockResults.push_back(new MockWriteResult(backend.shardHost, error));

    backend.setMockResults(mockResults);

    // Execute request
    BatchedCommandResponse response;
    backend.exec->executeBatch(request, &response);
    ASSERT(response.getOk());

    const BatchWriteExecStats& stats = backend.exec->getStats();
    ASSERT_EQUALS(stats.numStaleBatches, 1);
}

TEST(BatchWriteExecTests, MultiStaleOp) {
    //
    // Retry op in exec multiple times b/c of stale config
    //

    NamespaceString nss("foo.bar");

    // Insert request
    BatchedCommandRequest request(BatchedCommandRequest::BatchType_Insert);
    request.setNS(nss.ns());
    request.setOrdered(false);
    request.setWriteConcern(BSONObj());
    // Do single-target, single doc batch write op
    request.getInsertRequest()->addToDocuments(BSON("x" << 1));

    MockSingleShardBackend backend(nss);

    vector<MockWriteResult*> mockResults;
    WriteErrorDetail error;
    error.setErrCode(ErrorCodes::StaleShardVersion);
    error.setErrMessage("mock stale error");
    for (int i = 0; i < 3; i++) {
        mockResults.push_back(new MockWriteResult(backend.shardHost, error));
    }

    backend.setMockResults(mockResults);

    // Execute request
    BatchedCommandResponse response;
    backend.exec->executeBatch(request, &response);
    ASSERT(response.getOk());

    const BatchWriteExecStats& stats = backend.exec->getStats();
    ASSERT_EQUALS(stats.numStaleBatches, 3);
}

TEST(BatchWriteExecTests, TooManyStaleOp) {
    //
    // Retry op in exec too many times (without refresh) b/c of stale config
    // (The mock targeter doesn't report progress on refresh)
    // We should report a no progress error for everything in the batch
    //

    NamespaceString nss("foo.bar");

    // Insert request
    BatchedCommandRequest request(BatchedCommandRequest::BatchType_Insert);
    request.setNS(nss.ns());
    request.setOrdered(false);
    request.setWriteConcern(BSONObj());
    // Do single-target, single doc batch write ops
    request.getInsertRequest()->addToDocuments(BSON("x" << 1));
    request.getInsertRequest()->addToDocuments(BSON("x" << 2));

    MockSingleShardBackend backend(nss);

    vector<MockWriteResult*> mockResults;
    WriteErrorDetail error;
    error.setErrCode(ErrorCodes::StaleShardVersion);
    error.setErrMessage("mock stale error");
    for (int i = 0; i < 10; i++) {
        mockResults.push_back(
            new MockWriteResult(backend.shardHost, error, request.sizeWriteOps()));
    }

    backend.setMockResults(mockResults);

    // Execute request
    BatchedCommandResponse response;
    backend.exec->executeBatch(request, &response);
    ASSERT(response.getOk());
    ASSERT_EQUALS(response.getN(), 0);
    ASSERT(response.isErrDetailsSet());
    ASSERT_EQUALS(response.getErrDetailsAt(0)->getErrCode(), ErrorCodes::NoProgressMade);
    ASSERT_EQUALS(response.getErrDetailsAt(1)->getErrCode(), ErrorCodes::NoProgressMade);
}

TEST(BatchWriteExecTests, ManyStaleOpWithMigration) {
    //
    // Retry op in exec many times b/c of stale config, but simulate remote migrations occurring
    //

    NamespaceString nss("foo.bar");

    // Insert request
    BatchedCommandRequest request(BatchedCommandRequest::BatchType_Insert);
    request.setNS(nss.ns());
    request.setOrdered(false);
    request.setWriteConcern(BSONObj());
    // Do single-target, single doc batch write op
    request.getInsertRequest()->addToDocuments(BSON("x" << 1));

    MockSingleShardBackend backend(nss);

    vector<MockWriteResult*> mockResults;
    WriteErrorDetail error;
    error.setErrCode(ErrorCodes::StaleShardVersion);
    error.setErrMessage("mock stale error");
    for (int i = 0; i < 10; i++) {
        if (i % 2 == 0)
            error.setErrInfo(BSONObj());
        else
            error.setErrInfo(BSON("inCriticalSection" << true));

        mockResults.push_back(new MockWriteResult(backend.shardHost, error));
    }

    backend.setMockResults(mockResults);

    // Execute request
    BatchedCommandResponse response;
    backend.exec->executeBatch(request, &response);
    ASSERT(response.getOk());

    const BatchWriteExecStats& stats = backend.exec->getStats();
    ASSERT_EQUALS(stats.numStaleBatches, 10);
}

}  // unnamed namespace
