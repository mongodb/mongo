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

#include "mongo/base/owned_pointer_map.h"
#include "mongo/db/operation_context_noop.h"
#include "mongo/s/write_ops/batch_write_op.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/mock_ns_targeter.h"
#include "mongo/s/write_ops/write_error_detail.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

void initTargeterFullRange(const NamespaceString& nss,
                           const ShardEndpoint& endpoint,
                           MockNSTargeter* targeter) {
    targeter->init(nss, {MockRange(endpoint, BSON("x" << MINKEY), BSON("x" << MAXKEY))});
}

void initTargeterSplitRange(const NamespaceString& nss,
                            const ShardEndpoint& endpointA,
                            const ShardEndpoint& endpointB,
                            MockNSTargeter* targeter) {
    targeter->init(nss,
                   {MockRange(endpointA, BSON("x" << MINKEY), BSON("x" << 0)),
                    MockRange(endpointB, BSON("x" << 0), BSON("x" << MAXKEY))});
}

void initTargeterHalfRange(const NamespaceString& nss,
                           const ShardEndpoint& endpoint,
                           MockNSTargeter* targeter) {
    // x >= 0 values are untargetable
    targeter->init(nss, {MockRange(endpoint, BSON("x" << MINKEY), BSON("x" << 0))});
}

write_ops::DeleteOpEntry buildDelete(const BSONObj& query, bool multi) {
    write_ops::DeleteOpEntry entry;
    entry.setQ(query);
    entry.setMulti(multi);
    return entry;
}

write_ops::UpdateOpEntry buildUpdate(const BSONObj& query, bool multi) {
    write_ops::UpdateOpEntry entry;
    entry.setQ(query);
    entry.setU(BSONObj());
    entry.setMulti(multi);
    return entry;
}

write_ops::UpdateOpEntry buildUpdate(const BSONObj& query, const BSONObj& updateExpr, bool multi) {
    write_ops::UpdateOpEntry entry;
    entry.setQ(query);
    entry.setU(updateExpr);
    entry.setMulti(multi);
    return entry;
}

void buildResponse(int n, BatchedCommandResponse* response) {
    response->clear();
    response->setOk(true);
    response->setN(n);
    ASSERT(response->isValid(NULL));
}

void buildErrResponse(int code, const std::string& message, BatchedCommandResponse* response) {
    response->clear();
    response->setOk(false);
    response->setN(0);
    response->setErrCode(code);
    response->setErrMessage(message);
    ASSERT(response->isValid(NULL));
}

void addError(int code, const std::string& message, int index, BatchedCommandResponse* response) {
    std::unique_ptr<WriteErrorDetail> error(new WriteErrorDetail);
    error->setErrCode(code);
    error->setErrMessage(message);
    error->setIndex(index);

    response->addToErrDetails(error.release());
}

void addWCError(BatchedCommandResponse* response) {
    std::unique_ptr<WriteConcernErrorDetail> error(new WriteConcernErrorDetail);
    error->setErrCode(ErrorCodes::WriteConcernFailed);
    error->setErrMessage("mock wc error");

    response->setWriteConcernError(error.release());
}

class WriteOpTestFixture : public unittest::Test {
protected:
    OperationContext* operationContext() {
        return &_opCtx;
    }

private:
    OperationContextNoop _opCtx;
};

using BatchWriteOpTest = WriteOpTestFixture;

TEST_F(BatchWriteOpTest, SingleOp) {
    NamespaceString nss("foo.bar");
    ShardEndpoint endpoint(ShardId("shard"), ChunkVersion::IGNORED());
    MockNSTargeter targeter;
    initTargeterFullRange(nss, endpoint, &targeter);

    // Do single-target, single doc batch write op
    BatchedCommandRequest request([&] {
        write_ops::Insert insertOp(nss);
        insertOp.setDocuments({BSON("x" << 1)});
        return insertOp;
    }());

    BatchWriteOp batchOp(operationContext(), request);

    OwnedPointerMap<ShardId, TargetedWriteBatch> targetedOwned;
    std::map<ShardId, TargetedWriteBatch*>& targeted = targetedOwned.mutableMap();
    ASSERT_OK(batchOp.targetBatch(targeter, false, &targeted));
    ASSERT(!batchOp.isFinished());
    ASSERT_EQUALS(targeted.size(), 1u);
    assertEndpointsEqual(targeted.begin()->second->getEndpoint(), endpoint);

    BatchedCommandResponse response;
    buildResponse(1, &response);

    batchOp.noteBatchResponse(*targeted.begin()->second, response, NULL);
    ASSERT(batchOp.isFinished());

    BatchedCommandResponse clientResponse;
    batchOp.buildClientResponse(&clientResponse);
    ASSERT(clientResponse.getOk());
}

TEST_F(BatchWriteOpTest, SingleError) {
    NamespaceString nss("foo.bar");
    ShardEndpoint endpoint(ShardId("shard"), ChunkVersion::IGNORED());
    MockNSTargeter targeter;
    initTargeterFullRange(nss, endpoint, &targeter);

    // Do single-target, single doc batch write op
    BatchedCommandRequest request([&] {
        write_ops::Delete deleteOp(nss);
        deleteOp.setDeletes({buildDelete(BSON("x" << 1), false)});
        return deleteOp;
    }());

    BatchWriteOp batchOp(operationContext(), request);

    OwnedPointerMap<ShardId, TargetedWriteBatch> targetedOwned;
    std::map<ShardId, TargetedWriteBatch*>& targeted = targetedOwned.mutableMap();
    ASSERT_OK(batchOp.targetBatch(targeter, false, &targeted));
    ASSERT(!batchOp.isFinished());
    ASSERT_EQUALS(targeted.size(), 1u);
    assertEndpointsEqual(targeted.begin()->second->getEndpoint(), endpoint);

    BatchedCommandResponse response;
    buildErrResponse(ErrorCodes::UnknownError, "message", &response);

    batchOp.noteBatchResponse(*targeted.begin()->second, response, NULL);
    ASSERT(batchOp.isFinished());

    BatchedCommandResponse clientResponse;
    batchOp.buildClientResponse(&clientResponse);

    ASSERT(clientResponse.getOk());
    ASSERT_EQUALS(clientResponse.sizeErrDetails(), 1u);
    ASSERT_EQUALS(clientResponse.getErrDetailsAt(0)->getErrCode(), response.getErrCode());
    ASSERT(clientResponse.getErrDetailsAt(0)->getErrMessage().find(response.getErrMessage()) !=
           std::string::npos);
    ASSERT_EQUALS(clientResponse.getN(), 0);
}

TEST_F(BatchWriteOpTest, SingleTargetError) {
    NamespaceString nss("foo.bar");
    ShardEndpoint endpoint(ShardId("shard"), ChunkVersion::IGNORED());
    MockNSTargeter targeter;
    initTargeterHalfRange(nss, endpoint, &targeter);

    // Do untargetable delete op
    BatchedCommandRequest request([&] {
        write_ops::Delete deleteOp(nss);
        deleteOp.setDeletes({buildDelete(BSON("x" << 1), false)});
        return deleteOp;
    }());

    BatchWriteOp batchOp(operationContext(), request);

    OwnedPointerMap<ShardId, TargetedWriteBatch> targetedOwned;
    std::map<ShardId, TargetedWriteBatch*>& targeted = targetedOwned.mutableMap();
    ASSERT_NOT_OK(batchOp.targetBatch(targeter, false, &targeted));
    ASSERT(!batchOp.isFinished());
    ASSERT_EQUALS(targeted.size(), 0u);

    // Record targeting failures
    ASSERT_OK(batchOp.targetBatch(targeter, true, &targeted));
    ASSERT(batchOp.isFinished());
    ASSERT_EQUALS(targeted.size(), 0u);

    BatchedCommandResponse clientResponse;
    batchOp.buildClientResponse(&clientResponse);
    ASSERT(clientResponse.getOk());
    ASSERT_EQUALS(clientResponse.getN(), 0);
    ASSERT_EQUALS(clientResponse.sizeErrDetails(), 1u);
}

// Write concern error test - we should pass write concern to sub-batches, and pass up the write
// concern error if one occurs.
TEST_F(BatchWriteOpTest, SingleWriteConcernErrorOrdered) {
    NamespaceString nss("foo.bar");
    ShardEndpoint endpoint(ShardId("shard"), ChunkVersion::IGNORED());
    MockNSTargeter targeter;
    initTargeterFullRange(nss, endpoint, &targeter);

    BatchedCommandRequest request([&] {
        write_ops::Insert insertOp(nss);
        insertOp.setDocuments({BSON("x" << 1)});
        return insertOp;
    }());
    request.setWriteConcern(BSON("w" << 3));

    BatchWriteOp batchOp(operationContext(), request);

    OwnedPointerMap<ShardId, TargetedWriteBatch> targetedOwned;
    std::map<ShardId, TargetedWriteBatch*>& targeted = targetedOwned.mutableMap();
    ASSERT_OK(batchOp.targetBatch(targeter, false, &targeted));
    ASSERT(!batchOp.isFinished());
    ASSERT_EQUALS(targeted.size(), 1u);
    assertEndpointsEqual(targeted.begin()->second->getEndpoint(), endpoint);

    BatchedCommandRequest targetBatch = batchOp.buildBatchRequest(*targeted.begin()->second);
    ASSERT(targetBatch.getWriteConcern().woCompare(request.getWriteConcern()) == 0);

    BatchedCommandResponse response;
    buildResponse(1, &response);
    addWCError(&response);

    // First stale response comes back, we should retry
    batchOp.noteBatchResponse(*targeted.begin()->second, response, NULL);
    ASSERT(batchOp.isFinished());

    BatchedCommandResponse clientResponse;
    batchOp.buildClientResponse(&clientResponse);
    ASSERT(clientResponse.getOk());
    ASSERT_EQUALS(clientResponse.getN(), 1);
    ASSERT(!clientResponse.isErrDetailsSet());
    ASSERT(clientResponse.isWriteConcernErrorSet());
}

// Single-op stale version test. We should retry the same batch until we're not stale.
TEST_F(BatchWriteOpTest, SingleStaleError) {
    NamespaceString nss("foo.bar");
    ShardEndpoint endpoint(ShardId("shard"), ChunkVersion::IGNORED());
    MockNSTargeter targeter;
    initTargeterFullRange(nss, endpoint, &targeter);

    BatchedCommandRequest request([&] {
        write_ops::Insert insertOp(nss);
        insertOp.setDocuments({BSON("x" << 1)});
        return insertOp;
    }());

    BatchWriteOp batchOp(operationContext(), request);

    OwnedPointerMap<ShardId, TargetedWriteBatch> targetedOwned;
    std::map<ShardId, TargetedWriteBatch*>& targeted = targetedOwned.mutableMap();
    ASSERT_OK(batchOp.targetBatch(targeter, false, &targeted));

    BatchedCommandResponse response;
    buildResponse(0, &response);
    addError(ErrorCodes::StaleShardVersion, "mock stale error", 0, &response);

    // First stale response comes back, we should retry
    batchOp.noteBatchResponse(*targeted.begin()->second, response, NULL);
    ASSERT(!batchOp.isFinished());

    targetedOwned.clear();
    ASSERT_OK(batchOp.targetBatch(targeter, false, &targeted));

    // Respond again with a stale response
    batchOp.noteBatchResponse(*targeted.begin()->second, response, NULL);
    ASSERT(!batchOp.isFinished());

    targetedOwned.clear();
    ASSERT_OK(batchOp.targetBatch(targeter, false, &targeted));

    buildResponse(1, &response);

    // Respond with an 'ok' response
    batchOp.noteBatchResponse(*targeted.begin()->second, response, NULL);
    ASSERT(batchOp.isFinished());

    BatchedCommandResponse clientResponse;
    batchOp.buildClientResponse(&clientResponse);
    ASSERT(clientResponse.getOk());
    ASSERT_EQUALS(clientResponse.getN(), 1);
    ASSERT(!clientResponse.isErrDetailsSet());
}

//
// Multi-operation batches
//

// Multi-op targeting test (ordered)
TEST_F(BatchWriteOpTest, MultiOpSameShardOrdered) {
    NamespaceString nss("foo.bar");
    ShardEndpoint endpoint(ShardId("shard"), ChunkVersion::IGNORED());
    MockNSTargeter targeter;
    initTargeterFullRange(nss, endpoint, &targeter);

    // Do single-target, multi-doc batch write op
    BatchedCommandRequest request([&] {
        write_ops::Update updateOp(nss);
        updateOp.setUpdates(
            {buildUpdate(BSON("x" << 1), false), buildUpdate(BSON("x" << 2), false)});
        return updateOp;
    }());

    BatchWriteOp batchOp(operationContext(), request);

    OwnedPointerMap<ShardId, TargetedWriteBatch> targetedOwned;
    std::map<ShardId, TargetedWriteBatch*>& targeted = targetedOwned.mutableMap();
    ASSERT_OK(batchOp.targetBatch(targeter, false, &targeted));
    ASSERT(!batchOp.isFinished());
    ASSERT_EQUALS(targeted.size(), 1u);
    ASSERT_EQUALS(targeted.begin()->second->getWrites().size(), 2u);
    assertEndpointsEqual(targeted.begin()->second->getEndpoint(), endpoint);

    BatchedCommandResponse response;
    buildResponse(2, &response);

    batchOp.noteBatchResponse(*targeted.begin()->second, response, NULL);
    ASSERT(batchOp.isFinished());

    BatchedCommandResponse clientResponse;
    batchOp.buildClientResponse(&clientResponse);
    ASSERT(clientResponse.getOk());
    ASSERT_EQUALS(clientResponse.getN(), 2);
}

// Multi-op targeting test (unordered)
TEST_F(BatchWriteOpTest, MultiOpSameShardUnordered) {
    NamespaceString nss("foo.bar");
    ShardEndpoint endpoint(ShardId("shard"), ChunkVersion::IGNORED());
    MockNSTargeter targeter;
    initTargeterFullRange(nss, endpoint, &targeter);

    // Do single-target, multi-doc batch write op
    BatchedCommandRequest request([&] {
        write_ops::Update updateOp(nss);
        updateOp.setWriteCommandBase([] {
            write_ops::WriteCommandBase wcb;
            wcb.setOrdered(false);
            return wcb;
        }());
        updateOp.setUpdates(
            {buildUpdate(BSON("x" << 1), false), buildUpdate(BSON("x" << 2), false)});
        return updateOp;
    }());

    BatchWriteOp batchOp(operationContext(), request);

    OwnedPointerMap<ShardId, TargetedWriteBatch> targetedOwned;
    std::map<ShardId, TargetedWriteBatch*>& targeted = targetedOwned.mutableMap();
    ASSERT_OK(batchOp.targetBatch(targeter, false, &targeted));
    ASSERT(!batchOp.isFinished());
    ASSERT_EQUALS(targeted.size(), 1u);
    ASSERT_EQUALS(targeted.begin()->second->getWrites().size(), 2u);
    assertEndpointsEqual(targeted.begin()->second->getEndpoint(), endpoint);

    BatchedCommandResponse response;
    buildResponse(2, &response);

    batchOp.noteBatchResponse(*targeted.begin()->second, response, NULL);
    ASSERT(batchOp.isFinished());

    BatchedCommandResponse clientResponse;
    batchOp.buildClientResponse(&clientResponse);
    ASSERT(clientResponse.getOk());
    ASSERT_EQUALS(clientResponse.getN(), 2);
}

// Multi-op, multi-endpoing targeting test (ordered). There should be two sets of single batches
// (one to each shard, one-by-one)
TEST_F(BatchWriteOpTest, MultiOpTwoShardsOrdered) {
    NamespaceString nss("foo.bar");
    ShardEndpoint endpointA(ShardId("shardA"), ChunkVersion::IGNORED());
    ShardEndpoint endpointB(ShardId("shardB"), ChunkVersion::IGNORED());
    MockNSTargeter targeter;
    initTargeterSplitRange(nss, endpointA, endpointB, &targeter);

    // Do multi-target, multi-doc batch write op
    BatchedCommandRequest request([&] {
        write_ops::Insert insertOp(nss);
        insertOp.setDocuments({BSON("x" << -1), BSON("x" << 1)});
        return insertOp;
    }());

    BatchWriteOp batchOp(operationContext(), request);

    OwnedPointerMap<ShardId, TargetedWriteBatch> targetedOwned;
    std::map<ShardId, TargetedWriteBatch*>& targeted = targetedOwned.mutableMap();
    ASSERT_OK(batchOp.targetBatch(targeter, false, &targeted));
    ASSERT(!batchOp.isFinished());
    ASSERT_EQUALS(targeted.size(), 1u);
    ASSERT_EQUALS(targeted.begin()->second->getWrites().size(), 1u);
    assertEndpointsEqual(targeted.begin()->second->getEndpoint(), endpointA);

    BatchedCommandResponse response;
    buildResponse(1, &response);

    // Respond to first targeted batch
    batchOp.noteBatchResponse(*targeted.begin()->second, response, NULL);
    ASSERT(!batchOp.isFinished());

    targetedOwned.clear();

    ASSERT_OK(batchOp.targetBatch(targeter, false, &targeted));
    ASSERT(!batchOp.isFinished());
    ASSERT_EQUALS(targeted.size(), 1u);
    ASSERT_EQUALS(targeted.begin()->second->getWrites().size(), 1u);
    assertEndpointsEqual(targeted.begin()->second->getEndpoint(), endpointB);

    // Respond to second targeted batch
    batchOp.noteBatchResponse(*targeted.begin()->second, response, NULL);
    ASSERT(batchOp.isFinished());

    BatchedCommandResponse clientResponse;
    batchOp.buildClientResponse(&clientResponse);
    ASSERT(clientResponse.getOk());
    ASSERT_EQUALS(clientResponse.getN(), 2);
}

void verifyTargetedBatches(std::map<ShardId, size_t> expected,
                           const std::map<ShardId, TargetedWriteBatch*>& targeted) {
    // 'expected' contains each ShardId that was expected to be targeted and the size of the batch
    // that was expected to be targeted to it.
    // We check that each ShardId in 'targeted' corresponds to one in 'expected', in that it
    // contains a batch of the correct size.
    // Finally, we ensure that no additional ShardIds are present in 'targeted' than 'expected'.
    for (auto it = targeted.begin(); it != targeted.end(); ++it) {
        ASSERT_EQUALS(expected[it->second->getEndpoint().shardName],
                      it->second->getWrites().size());
        ASSERT_EQUALS(ChunkVersion::IGNORED(), it->second->getEndpoint().shardVersion);
        expected.erase(expected.find(it->second->getEndpoint().shardName));
    }
    ASSERT(expected.empty());
}

// Multi-op, multi-endpoint targeting test (unordered). There should be one set of two batches (one
// to each shard).
TEST_F(BatchWriteOpTest, MultiOpTwoShardsUnordered) {
    NamespaceString nss("foo.bar");
    ShardEndpoint endpointA(ShardId("shardA"), ChunkVersion::IGNORED());
    ShardEndpoint endpointB(ShardId("shardB"), ChunkVersion::IGNORED());
    MockNSTargeter targeter;
    initTargeterSplitRange(nss, endpointA, endpointB, &targeter);

    // Do multi-target, multi-doc batch write op
    BatchedCommandRequest request([&] {
        write_ops::Insert insertOp(nss);
        insertOp.setWriteCommandBase([] {
            write_ops::WriteCommandBase wcb;
            wcb.setOrdered(false);
            return wcb;
        }());
        insertOp.setDocuments({BSON("x" << -1), BSON("x" << 1)});
        return insertOp;
    }());

    BatchWriteOp batchOp(operationContext(), request);

    OwnedPointerMap<ShardId, TargetedWriteBatch> targetedOwned;
    std::map<ShardId, TargetedWriteBatch*>& targeted = targetedOwned.mutableMap();
    ASSERT_OK(batchOp.targetBatch(targeter, false, &targeted));
    ASSERT(!batchOp.isFinished());
    ASSERT_EQUALS(targeted.size(), 2u);
    verifyTargetedBatches({{endpointA.shardName, 1u}, {endpointB.shardName, 1u}}, targeted);

    BatchedCommandResponse response;
    buildResponse(1, &response);

    // Respond to both targeted batches
    for (auto it = targeted.begin(); it != targeted.end(); ++it) {
        ASSERT(!batchOp.isFinished());
        batchOp.noteBatchResponse(*it->second, response, NULL);
    }
    ASSERT(batchOp.isFinished());

    BatchedCommandResponse clientResponse;
    batchOp.buildClientResponse(&clientResponse);
    ASSERT(clientResponse.getOk());
    ASSERT_EQUALS(clientResponse.getN(), 2);
}

// Multi-op (ordered) targeting test where each op goes to both shards. There should be two sets of
// two batches to each shard (two for each delete op).
TEST_F(BatchWriteOpTest, MultiOpTwoShardsEachOrdered) {
    NamespaceString nss("foo.bar");
    ShardEndpoint endpointA(ShardId("shardA"), ChunkVersion::IGNORED());
    ShardEndpoint endpointB(ShardId("shardB"), ChunkVersion::IGNORED());
    MockNSTargeter targeter;
    initTargeterSplitRange(nss, endpointA, endpointB, &targeter);

    // Do multi-target, multi-doc batch write op
    BatchedCommandRequest request([&] {
        write_ops::Delete deleteOp(nss);
        deleteOp.setDeletes({buildDelete(BSON("x" << GTE << -1 << LT << 2), true),
                             buildDelete(BSON("x" << GTE << -2 << LT << 1), true)});
        return deleteOp;
    }());

    BatchWriteOp batchOp(operationContext(), request);

    OwnedPointerMap<ShardId, TargetedWriteBatch> targetedOwned;
    std::map<ShardId, TargetedWriteBatch*>& targeted = targetedOwned.mutableMap();
    ASSERT_OK(batchOp.targetBatch(targeter, false, &targeted));
    ASSERT(!batchOp.isFinished());
    ASSERT_EQUALS(targeted.size(), 2u);
    verifyTargetedBatches({{endpointA.shardName, 1u}, {endpointB.shardName, 1u}}, targeted);

    BatchedCommandResponse response;
    buildResponse(1, &response);

    // Respond to both targeted batches for first multi-delete
    for (auto it = targeted.begin(); it != targeted.end(); ++it) {
        ASSERT(!batchOp.isFinished());
        batchOp.noteBatchResponse(*it->second, response, NULL);
    }
    ASSERT(!batchOp.isFinished());

    targetedOwned.clear();

    ASSERT_OK(batchOp.targetBatch(targeter, false, &targeted));
    ASSERT(!batchOp.isFinished());
    ASSERT_EQUALS(targeted.size(), 2u);
    verifyTargetedBatches({{endpointA.shardName, 1u}, {endpointB.shardName, 1u}}, targeted);

    // Respond to second targeted batches for second multi-delete
    for (auto it = targeted.begin(); it != targeted.end(); ++it) {
        ASSERT(!batchOp.isFinished());
        batchOp.noteBatchResponse(*it->second, response, NULL);
    }
    ASSERT(batchOp.isFinished());

    BatchedCommandResponse clientResponse;
    batchOp.buildClientResponse(&clientResponse);
    ASSERT(clientResponse.getOk());
    ASSERT_EQUALS(clientResponse.getN(), 4);
}

// Multi-op (unaordered) targeting test where each op goes to both shards. There should be one set
// of two batches to each shard (containing writes for both ops).
TEST_F(BatchWriteOpTest, MultiOpTwoShardsEachUnordered) {
    NamespaceString nss("foo.bar");
    ShardEndpoint endpointA(ShardId("shardA"), ChunkVersion::IGNORED());
    ShardEndpoint endpointB(ShardId("shardB"), ChunkVersion::IGNORED());
    MockNSTargeter targeter;
    initTargeterSplitRange(nss, endpointA, endpointB, &targeter);

    // Do multi-target, multi-doc batch write op
    BatchedCommandRequest request([&] {
        write_ops::Update updateOp(nss);
        updateOp.setWriteCommandBase([] {
            write_ops::WriteCommandBase wcb;
            wcb.setOrdered(false);
            return wcb;
        }());
        updateOp.setUpdates({buildUpdate(BSON("x" << GTE << -1 << LT << 2), true),
                             buildUpdate(BSON("x" << GTE << -2 << LT << 1), true)});
        return updateOp;
    }());

    BatchWriteOp batchOp(operationContext(), request);

    OwnedPointerMap<ShardId, TargetedWriteBatch> targetedOwned;
    std::map<ShardId, TargetedWriteBatch*>& targeted = targetedOwned.mutableMap();
    ASSERT_OK(batchOp.targetBatch(targeter, false, &targeted));
    ASSERT(!batchOp.isFinished());
    ASSERT_EQUALS(targeted.size(), 2u);
    verifyTargetedBatches({{endpointA.shardName, 2u}, {endpointB.shardName, 2u}}, targeted);

    BatchedCommandResponse response;
    buildResponse(2, &response);

    // Respond to both targeted batches, each containing two ops
    for (auto it = targeted.begin(); it != targeted.end(); ++it) {
        ASSERT(!batchOp.isFinished());
        batchOp.noteBatchResponse(*it->second, response, NULL);
    }
    ASSERT(batchOp.isFinished());

    BatchedCommandResponse clientResponse;
    batchOp.buildClientResponse(&clientResponse);
    ASSERT(clientResponse.getOk());
    ASSERT_EQUALS(clientResponse.getN(), 4);
}

// Multi-op (ordered) targeting test where first two ops go to one shard, second two ops go to two
// shards. Should batch the first two ops, then second ops should be batched separately, then last
// ops should be batched together.
TEST_F(BatchWriteOpTest, MultiOpOneOrTwoShardsOrdered) {
    NamespaceString nss("foo.bar");
    ShardEndpoint endpointA(ShardId("shardA"), ChunkVersion::IGNORED());
    ShardEndpoint endpointB(ShardId("shardB"), ChunkVersion::IGNORED());
    MockNSTargeter targeter;
    initTargeterSplitRange(nss, endpointA, endpointB, &targeter);

    BatchedCommandRequest request([&] {
        write_ops::Delete deleteOp(nss);
        deleteOp.setDeletes({
            // These go to the same shard
            buildDelete(BSON("x" << -1), false),
            buildDelete(BSON("x" << -2), false),
            // These go to both shards
            buildDelete(BSON("x" << GTE << -1 << LT << 2), true),
            buildDelete(BSON("x" << GTE << -2 << LT << 1), true),
            // These go to the same shard
            buildDelete(BSON("x" << 1), false),
            buildDelete(BSON("x" << 2), false),
        });
        return deleteOp;
    }());

    BatchWriteOp batchOp(operationContext(), request);

    OwnedPointerMap<ShardId, TargetedWriteBatch> targetedOwned;
    std::map<ShardId, TargetedWriteBatch*>& targeted = targetedOwned.mutableMap();
    ASSERT_OK(batchOp.targetBatch(targeter, false, &targeted));
    ASSERT(!batchOp.isFinished());
    ASSERT_EQUALS(targeted.size(), 1u);
    ASSERT_EQUALS(targeted.begin()->second->getWrites().size(), 2u);
    assertEndpointsEqual(targeted.begin()->second->getEndpoint(), endpointA);

    BatchedCommandResponse response;
    // Emulate one-write-per-delete-per-host
    buildResponse(2, &response);

    // Respond to first targeted batch containing the two single-host deletes
    batchOp.noteBatchResponse(*targeted.begin()->second, response, NULL);
    ASSERT(!batchOp.isFinished());

    targetedOwned.clear();

    ASSERT_OK(batchOp.targetBatch(targeter, false, &targeted));
    ASSERT(!batchOp.isFinished());
    ASSERT_EQUALS(targeted.size(), 2u);
    verifyTargetedBatches({{endpointA.shardName, 1u}, {endpointB.shardName, 1u}}, targeted);

    // Emulate one-write-per-delete-per-host
    buildResponse(1, &response);

    // Respond to two targeted batches for first multi-delete
    for (auto it = targeted.begin(); it != targeted.end(); ++it) {
        ASSERT(!batchOp.isFinished());
        batchOp.noteBatchResponse(*it->second, response, NULL);
    }
    ASSERT(!batchOp.isFinished());

    targetedOwned.clear();

    ASSERT_OK(batchOp.targetBatch(targeter, false, &targeted));
    ASSERT(!batchOp.isFinished());
    ASSERT_EQUALS(targeted.size(), 2u);
    verifyTargetedBatches({{endpointA.shardName, 1u}, {endpointB.shardName, 1u}}, targeted);

    // Respond to two targeted batches for second multi-delete
    for (auto it = targeted.begin(); it != targeted.end(); ++it) {
        ASSERT(!batchOp.isFinished());
        batchOp.noteBatchResponse(*it->second, response, NULL);
    }
    ASSERT(!batchOp.isFinished());

    targetedOwned.clear();

    ASSERT_OK(batchOp.targetBatch(targeter, false, &targeted));
    ASSERT(!batchOp.isFinished());
    ASSERT_EQUALS(targeted.size(), 1u);
    ASSERT_EQUALS(targeted.begin()->second->getWrites().size(), 2u);
    assertEndpointsEqual(targeted.begin()->second->getEndpoint(), endpointB);

    // Emulate one-write-per-delete-per-host
    buildResponse(2, &response);

    // Respond to final targeted batch containing the last two single-host deletes
    batchOp.noteBatchResponse(*targeted.begin()->second, response, NULL);
    ASSERT(batchOp.isFinished());

    BatchedCommandResponse clientResponse;
    batchOp.buildClientResponse(&clientResponse);
    ASSERT(clientResponse.getOk());
    ASSERT_EQUALS(clientResponse.getN(), 8);
}

// Multi-op (unordered) targeting test where first two ops go to one shard, second two ops go to two
// shards. Should batch all the ops together into two batches of four ops for each shard.
TEST_F(BatchWriteOpTest, MultiOpOneOrTwoShardsUnordered) {
    NamespaceString nss("foo.bar");
    ShardEndpoint endpointA(ShardId("shardA"), ChunkVersion::IGNORED());
    ShardEndpoint endpointB(ShardId("shardB"), ChunkVersion::IGNORED());
    MockNSTargeter targeter;
    initTargeterSplitRange(nss, endpointA, endpointB, &targeter);

    BatchedCommandRequest request([&] {
        write_ops::Update updateOp(nss);
        updateOp.setWriteCommandBase([] {
            write_ops::WriteCommandBase wcb;
            wcb.setOrdered(false);
            return wcb;
        }());
        updateOp.setUpdates({// These go to the same shard
                             buildUpdate(BSON("x" << -1), false),
                             buildUpdate(BSON("x" << -2), false),
                             // These go to both shards
                             buildUpdate(BSON("x" << GTE << -1 << LT << 2), true),
                             buildUpdate(BSON("x" << GTE << -2 << LT << 1), true),
                             // These go to the same shard
                             buildUpdate(BSON("x" << 1), false),
                             buildUpdate(BSON("x" << 2), false)});
        return updateOp;
    }());

    BatchWriteOp batchOp(operationContext(), request);

    OwnedPointerMap<ShardId, TargetedWriteBatch> targetedOwned;
    std::map<ShardId, TargetedWriteBatch*>& targeted = targetedOwned.mutableMap();
    ASSERT_OK(batchOp.targetBatch(targeter, false, &targeted));
    ASSERT(!batchOp.isFinished());
    ASSERT_EQUALS(targeted.size(), 2u);
    verifyTargetedBatches({{endpointA.shardName, 4u}, {endpointB.shardName, 4u}}, targeted);

    BatchedCommandResponse response;
    // Emulate one-write-per-delete-per-host
    buildResponse(4, &response);

    // Respond to first targeted batch containing the two single-host deletes
    for (auto it = targeted.begin(); it != targeted.end(); ++it) {
        ASSERT(!batchOp.isFinished());
        batchOp.noteBatchResponse(*it->second, response, NULL);
    }
    ASSERT(batchOp.isFinished());

    BatchedCommandResponse clientResponse;
    batchOp.buildClientResponse(&clientResponse);
    ASSERT(clientResponse.getOk());
    ASSERT_EQUALS(clientResponse.getN(), 8);
}

// Multi-op targeting test where two ops go to two separate shards and there's an error on one op on
// one shard. There should be one set of two batches to each shard and an error reported.
TEST_F(BatchWriteOpTest, MultiOpSingleShardErrorUnordered) {
    NamespaceString nss("foo.bar");
    ShardEndpoint endpointA(ShardId("shardA"), ChunkVersion::IGNORED());
    ShardEndpoint endpointB(ShardId("shardB"), ChunkVersion::IGNORED());
    MockNSTargeter targeter;
    initTargeterSplitRange(nss, endpointA, endpointB, &targeter);

    BatchedCommandRequest request([&] {
        write_ops::Insert insertOp(nss);
        insertOp.setWriteCommandBase([] {
            write_ops::WriteCommandBase wcb;
            wcb.setOrdered(false);
            return wcb;
        }());
        insertOp.setDocuments({BSON("x" << -1), BSON("x" << 1)});
        return insertOp;
    }());

    BatchWriteOp batchOp(operationContext(), request);

    OwnedPointerMap<ShardId, TargetedWriteBatch> targetedOwned;
    std::map<ShardId, TargetedWriteBatch*>& targeted = targetedOwned.mutableMap();
    ASSERT_OK(batchOp.targetBatch(targeter, false, &targeted));
    ASSERT(!batchOp.isFinished());
    ASSERT_EQUALS(targeted.size(), 2u);
    verifyTargetedBatches({{endpointA.shardName, 1u}, {endpointB.shardName, 1u}}, targeted);

    BatchedCommandResponse response;
    buildResponse(1, &response);

    // Respond to batches.
    auto targetedIt = targeted.begin();

    // No error on first shard
    batchOp.noteBatchResponse(*targetedIt->second, response, NULL);
    ASSERT(!batchOp.isFinished());

    buildResponse(0, &response);
    addError(ErrorCodes::UnknownError, "mock error", 0, &response);

    // Error on second write on second shard
    ++targetedIt;
    batchOp.noteBatchResponse(*targetedIt->second, response, NULL);
    ASSERT(batchOp.isFinished());
    ASSERT(++targetedIt == targeted.end());

    BatchedCommandResponse clientResponse;
    batchOp.buildClientResponse(&clientResponse);
    ASSERT(clientResponse.getOk());
    ASSERT_EQUALS(clientResponse.getN(), 1);
    ASSERT(clientResponse.isErrDetailsSet());
    ASSERT_EQUALS(clientResponse.sizeErrDetails(), 1u);
    ASSERT_EQUALS(clientResponse.getErrDetailsAt(0)->getErrCode(),
                  response.getErrDetailsAt(0)->getErrCode());
    ASSERT_EQUALS(clientResponse.getErrDetailsAt(0)->getErrMessage(),
                  response.getErrDetailsAt(0)->getErrMessage());
    ASSERT_EQUALS(clientResponse.getErrDetailsAt(0)->getIndex(), 1);
}

// Multi-op targeting test where two ops go to two separate shards and there's an error on each op
// on each shard. There should be one set of two batches to each shard and and two errors reported.
TEST_F(BatchWriteOpTest, MultiOpTwoShardErrorsUnordered) {
    NamespaceString nss("foo.bar");
    ShardEndpoint endpointA(ShardId("shardA"), ChunkVersion::IGNORED());
    ShardEndpoint endpointB(ShardId("shardB"), ChunkVersion::IGNORED());
    MockNSTargeter targeter;
    initTargeterSplitRange(nss, endpointA, endpointB, &targeter);

    BatchedCommandRequest request([&] {
        write_ops::Insert insertOp(nss);
        insertOp.setWriteCommandBase([] {
            write_ops::WriteCommandBase wcb;
            wcb.setOrdered(false);
            return wcb;
        }());
        insertOp.setDocuments({BSON("x" << -1), BSON("x" << 1)});
        return insertOp;
    }());

    BatchWriteOp batchOp(operationContext(), request);

    OwnedPointerMap<ShardId, TargetedWriteBatch> targetedOwned;
    std::map<ShardId, TargetedWriteBatch*>& targeted = targetedOwned.mutableMap();
    ASSERT_OK(batchOp.targetBatch(targeter, false, &targeted));
    ASSERT(!batchOp.isFinished());
    ASSERT_EQUALS(targeted.size(), 2u);
    verifyTargetedBatches({{endpointA.shardName, 1u}, {endpointB.shardName, 1u}}, targeted);

    BatchedCommandResponse response;
    buildResponse(0, &response);
    addError(ErrorCodes::UnknownError, "mock error", 0, &response);

    // Error on first write on first shard and second write on second shard.
    for (auto it = targeted.begin(); it != targeted.end(); ++it) {
        ASSERT(!batchOp.isFinished());
        batchOp.noteBatchResponse(*it->second, response, NULL);
    }
    ASSERT(batchOp.isFinished());

    BatchedCommandResponse clientResponse;
    batchOp.buildClientResponse(&clientResponse);
    ASSERT(clientResponse.getOk());
    ASSERT_EQUALS(clientResponse.getN(), 0);
    ASSERT(clientResponse.isErrDetailsSet());
    ASSERT_EQUALS(clientResponse.sizeErrDetails(), 2u);
    ASSERT_EQUALS(clientResponse.getErrDetailsAt(0)->getErrCode(),
                  response.getErrDetailsAt(0)->getErrCode());
    ASSERT_EQUALS(clientResponse.getErrDetailsAt(0)->getErrMessage(),
                  response.getErrDetailsAt(0)->getErrMessage());
    ASSERT_EQUALS(clientResponse.getErrDetailsAt(0)->getIndex(), 0);
    ASSERT_EQUALS(clientResponse.getErrDetailsAt(1)->getErrCode(),
                  response.getErrDetailsAt(0)->getErrCode());
    ASSERT_EQUALS(clientResponse.getErrDetailsAt(1)->getErrMessage(),
                  response.getErrDetailsAt(0)->getErrMessage());
    ASSERT_EQUALS(clientResponse.getErrDetailsAt(1)->getIndex(), 1);
}

// Multi-op targeting test where each op goes to both shards and there's an error on one op on one
// shard. There should be one set of two batches to each shard and an error reported.
TEST_F(BatchWriteOpTest, MultiOpPartialSingleShardErrorUnordered) {
    NamespaceString nss("foo.bar");
    ShardEndpoint endpointA(ShardId("shardA"), ChunkVersion::IGNORED());
    ShardEndpoint endpointB(ShardId("shardB"), ChunkVersion::IGNORED());
    MockNSTargeter targeter;
    initTargeterSplitRange(nss, endpointA, endpointB, &targeter);

    BatchedCommandRequest request([&] {
        write_ops::Delete deleteOp(nss);
        deleteOp.setWriteCommandBase([] {
            write_ops::WriteCommandBase wcb;
            wcb.setOrdered(false);
            return wcb;
        }());
        deleteOp.setDeletes({buildDelete(BSON("x" << GTE << -1 << LT << 2), true),
                             buildDelete(BSON("x" << GTE << -2 << LT << 1), true)});
        return deleteOp;
    }());

    BatchWriteOp batchOp(operationContext(), request);

    OwnedPointerMap<ShardId, TargetedWriteBatch> targetedOwned;
    std::map<ShardId, TargetedWriteBatch*>& targeted = targetedOwned.mutableMap();
    ASSERT_OK(batchOp.targetBatch(targeter, false, &targeted));
    ASSERT(!batchOp.isFinished());
    ASSERT_EQUALS(targeted.size(), 2u);
    verifyTargetedBatches({{endpointA.shardName, 2u}, {endpointB.shardName, 2u}}, targeted);

    // Respond to batches.
    auto targetedIt = targeted.begin();

    BatchedCommandResponse response;
    buildResponse(2, &response);

    // No errors on first shard
    batchOp.noteBatchResponse(*targetedIt->second, response, NULL);
    ASSERT(!batchOp.isFinished());

    buildResponse(1, &response);
    addError(ErrorCodes::UnknownError, "mock error", 1, &response);

    // Error on second write on second shard
    ++targetedIt;
    batchOp.noteBatchResponse(*targetedIt->second, response, NULL);
    ASSERT(batchOp.isFinished());
    ASSERT(++targetedIt == targeted.end());

    BatchedCommandResponse clientResponse;
    batchOp.buildClientResponse(&clientResponse);
    ASSERT(clientResponse.getOk());
    ASSERT_EQUALS(clientResponse.getN(), 3);
    ASSERT(clientResponse.isErrDetailsSet());
    ASSERT_EQUALS(clientResponse.sizeErrDetails(), 1u);
    ASSERT_EQUALS(clientResponse.getErrDetailsAt(0)->getErrCode(),
                  response.getErrDetailsAt(0)->getErrCode());
    ASSERT_EQUALS(clientResponse.getErrDetailsAt(0)->getErrMessage(),
                  response.getErrDetailsAt(0)->getErrMessage());
    ASSERT_EQUALS(clientResponse.getErrDetailsAt(0)->getIndex(), 1);
}

// Multi-op targeting test where each op goes to both shards and there's an error on one op on one
// shard. There should be one set of two batches to each shard and an error reported, the second op
// should not get run.
TEST_F(BatchWriteOpTest, MultiOpPartialSingleShardErrorOrdered) {
    NamespaceString nss("foo.bar");
    ShardEndpoint endpointA(ShardId("shardA"), ChunkVersion::IGNORED());
    ShardEndpoint endpointB(ShardId("shardB"), ChunkVersion::IGNORED());
    MockNSTargeter targeter;
    initTargeterSplitRange(nss, endpointA, endpointB, &targeter);

    BatchedCommandRequest request([&] {
        write_ops::Delete deleteOp(nss);
        deleteOp.setDeletes({buildDelete(BSON("x" << GTE << -1 << LT << 2), true),
                             buildDelete(BSON("x" << GTE << -2 << LT << 1), true)});
        return deleteOp;
    }());

    BatchWriteOp batchOp(operationContext(), request);

    OwnedPointerMap<ShardId, TargetedWriteBatch> targetedOwned;
    std::map<ShardId, TargetedWriteBatch*>& targeted = targetedOwned.mutableMap();
    ASSERT_OK(batchOp.targetBatch(targeter, false, &targeted));
    ASSERT(!batchOp.isFinished());
    ASSERT_EQUALS(targeted.size(), 2u);
    verifyTargetedBatches({{endpointA.shardName, 1u}, {endpointB.shardName, 1u}}, targeted);

    // Respond to batches.
    auto targetedIt = targeted.begin();

    BatchedCommandResponse response;
    buildResponse(1, &response);

    // No errors on first shard
    batchOp.noteBatchResponse(*targetedIt->second, response, NULL);
    ASSERT(!batchOp.isFinished());

    buildResponse(0, &response);
    addError(ErrorCodes::UnknownError, "mock error", 0, &response);

    // Error on second write on second shard
    ++targetedIt;
    batchOp.noteBatchResponse(*targetedIt->second, response, NULL);
    ASSERT(batchOp.isFinished());
    ASSERT(++targetedIt == targeted.end());

    BatchedCommandResponse clientResponse;
    batchOp.buildClientResponse(&clientResponse);
    ASSERT(clientResponse.getOk());
    ASSERT_EQUALS(clientResponse.getN(), 1);
    ASSERT(clientResponse.isErrDetailsSet());
    ASSERT_EQUALS(clientResponse.sizeErrDetails(), 1u);
    ASSERT_EQUALS(clientResponse.getErrDetailsAt(0)->getErrCode(),
                  response.getErrDetailsAt(0)->getErrCode());
    ASSERT_EQUALS(clientResponse.getErrDetailsAt(0)->getErrMessage(),
                  response.getErrDetailsAt(0)->getErrMessage());
    ASSERT_EQUALS(clientResponse.getErrDetailsAt(0)->getIndex(), 0);
}

//
// Tests of edge-case functionality, lifecycle is assumed to be behaving normally
//

// Multi-op (unordered) error and write concern error test. We never report the write concern error
// for single-doc batches, since the error means there's no write concern applied. Don't suppress
// the error if ordered : false.
TEST_F(BatchWriteOpTest, MultiOpErrorAndWriteConcernErrorUnordered) {
    NamespaceString nss("foo.bar");
    ShardEndpoint endpoint(ShardId("shard"), ChunkVersion::IGNORED());
    MockNSTargeter targeter;
    initTargeterFullRange(nss, endpoint, &targeter);

    BatchedCommandRequest request([&] {
        write_ops::Insert insertOp(nss);
        insertOp.setWriteCommandBase([] {
            write_ops::WriteCommandBase wcb;
            wcb.setOrdered(false);
            return wcb;
        }());
        insertOp.setDocuments({BSON("x" << 1), BSON("x" << 1)});
        return insertOp;
    }());
    request.setWriteConcern(BSON("w" << 3));

    BatchWriteOp batchOp(operationContext(), request);

    OwnedPointerMap<ShardId, TargetedWriteBatch> targetedOwned;
    std::map<ShardId, TargetedWriteBatch*>& targeted = targetedOwned.mutableMap();
    ASSERT_OK(batchOp.targetBatch(targeter, false, &targeted));

    BatchedCommandResponse response;
    buildResponse(1, &response);
    addError(ErrorCodes::UnknownError, "mock error", 1, &response);
    addWCError(&response);

    // First stale response comes back, we should retry
    batchOp.noteBatchResponse(*targeted.begin()->second, response, NULL);
    ASSERT(batchOp.isFinished());

    // Unordered reports write concern error
    BatchedCommandResponse clientResponse;
    batchOp.buildClientResponse(&clientResponse);
    ASSERT(clientResponse.getOk());
    ASSERT_EQUALS(clientResponse.getN(), 1);
    ASSERT_EQUALS(clientResponse.sizeErrDetails(), 1u);
    ASSERT(clientResponse.isWriteConcernErrorSet());
}

// Single-op (ordered) error and write concern error test. Suppress the write concern error if
// ordered and we also have an error
TEST_F(BatchWriteOpTest, SingleOpErrorAndWriteConcernErrorOrdered) {
    NamespaceString nss("foo.bar");
    ShardEndpoint endpointA(ShardId("shardA"), ChunkVersion::IGNORED());
    ShardEndpoint endpointB(ShardId("shardB"), ChunkVersion::IGNORED());
    MockNSTargeter targeter;
    initTargeterSplitRange(nss, endpointA, endpointB, &targeter);

    BatchedCommandRequest request([&] {
        write_ops::Update updateOp(nss);
        updateOp.setWriteCommandBase([] {
            write_ops::WriteCommandBase wcb;
            wcb.setOrdered(false);
            return wcb;
        }());
        updateOp.setUpdates({buildUpdate(BSON("x" << GTE << -1 << LT << 2), true)});
        return updateOp;
    }());
    request.setWriteConcern(BSON("w" << 3));

    BatchWriteOp batchOp(operationContext(), request);

    OwnedPointerMap<ShardId, TargetedWriteBatch> targetedOwned;
    std::map<ShardId, TargetedWriteBatch*>& targeted = targetedOwned.mutableMap();
    ASSERT_OK(batchOp.targetBatch(targeter, false, &targeted));

    // Respond to batches.
    auto targetedIt = targeted.begin();

    BatchedCommandResponse response;
    buildResponse(1, &response);
    addWCError(&response);

    // First response comes back with write concern error
    batchOp.noteBatchResponse(*targetedIt->second, response, NULL);
    ASSERT(!batchOp.isFinished());

    buildResponse(0, &response);
    addError(ErrorCodes::UnknownError, "mock error", 0, &response);

    // Second response comes back with write error
    ++targetedIt;
    batchOp.noteBatchResponse(*targetedIt->second, response, NULL);
    ASSERT(batchOp.isFinished());
    ASSERT(++targetedIt == targeted.end());

    // Ordered doesn't report write concern error
    BatchedCommandResponse clientResponse;
    batchOp.buildClientResponse(&clientResponse);
    ASSERT(clientResponse.getOk());
    ASSERT_EQUALS(clientResponse.getN(), 1);
    ASSERT(clientResponse.isErrDetailsSet());
    ASSERT_EQUALS(clientResponse.sizeErrDetails(), 1u);
    ASSERT(!clientResponse.isWriteConcernErrorSet());
}

// Targeting failure on second op in batch op (ordered)
TEST_F(BatchWriteOpTest, MultiOpFailedTargetOrdered) {
    NamespaceString nss("foo.bar");
    ShardEndpoint endpoint(ShardId("shard"), ChunkVersion::IGNORED());
    MockNSTargeter targeter;
    initTargeterHalfRange(nss, endpoint, &targeter);

    BatchedCommandRequest request([&] {
        write_ops::Insert insertOp(nss);
        insertOp.setDocuments({BSON("x" << -1), BSON("x" << 2), BSON("x" << -2)});
        return insertOp;
    }());

    // Do single-target, multi-doc batch write op

    BatchWriteOp batchOp(operationContext(), request);

    OwnedPointerMap<ShardId, TargetedWriteBatch> targetedOwned;
    std::map<ShardId, TargetedWriteBatch*>& targeted = targetedOwned.mutableMap();
    ASSERT_NOT_OK(batchOp.targetBatch(targeter, false, &targeted));

    // First targeting round fails since we may be stale
    ASSERT(!batchOp.isFinished());

    targetedOwned.clear();
    ASSERT_OK(batchOp.targetBatch(targeter, true, &targeted));

    // Second targeting round is ok, but should stop at first write
    ASSERT(!batchOp.isFinished());
    ASSERT_EQUALS(targeted.size(), 1u);
    ASSERT_EQUALS(targeted.begin()->second->getWrites().size(), 1u);

    BatchedCommandResponse response;
    buildResponse(1, &response);

    // First response ok
    batchOp.noteBatchResponse(*targeted.begin()->second, response, NULL);
    ASSERT(!batchOp.isFinished());

    targetedOwned.clear();
    ASSERT_OK(batchOp.targetBatch(targeter, true, &targeted));

    // Second targeting round results in an error which finishes the batch
    ASSERT(batchOp.isFinished());
    ASSERT_EQUALS(targeted.size(), 0u);

    BatchedCommandResponse clientResponse;
    batchOp.buildClientResponse(&clientResponse);
    ASSERT(clientResponse.getOk());
    ASSERT_EQUALS(clientResponse.getN(), 1);
    ASSERT(clientResponse.isErrDetailsSet());
    ASSERT_EQUALS(clientResponse.sizeErrDetails(), 1u);
    ASSERT_EQUALS(clientResponse.getErrDetailsAt(0)->getIndex(), 1);
}

// Targeting failure on second op in batch op (unordered)
TEST_F(BatchWriteOpTest, MultiOpFailedTargetUnordered) {
    NamespaceString nss("foo.bar");
    ShardEndpoint endpoint(ShardId("shard"), ChunkVersion::IGNORED());
    MockNSTargeter targeter;
    initTargeterHalfRange(nss, endpoint, &targeter);

    BatchedCommandRequest request([&] {
        write_ops::Insert insertOp(nss);
        insertOp.setWriteCommandBase([] {
            write_ops::WriteCommandBase wcb;
            wcb.setOrdered(false);
            return wcb;
        }());
        insertOp.setDocuments({BSON("x" << -1), BSON("x" << 2), BSON("x" << -2)});
        return insertOp;
    }());

    // Do single-target, multi-doc batch write op

    BatchWriteOp batchOp(operationContext(), request);

    OwnedPointerMap<ShardId, TargetedWriteBatch> targetedOwned;
    std::map<ShardId, TargetedWriteBatch*>& targeted = targetedOwned.mutableMap();
    ASSERT_NOT_OK(batchOp.targetBatch(targeter, false, &targeted));

    // First targeting round fails since we may be stale
    ASSERT(!batchOp.isFinished());

    targetedOwned.clear();
    ASSERT_OK(batchOp.targetBatch(targeter, true, &targeted));

    // Second targeting round is ok, and should record an error
    ASSERT(!batchOp.isFinished());
    ASSERT_EQUALS(targeted.size(), 1u);
    ASSERT_EQUALS(targeted.begin()->second->getWrites().size(), 2u);

    BatchedCommandResponse response;
    buildResponse(2, &response);

    // Response is ok for first and third write
    batchOp.noteBatchResponse(*targeted.begin()->second, response, NULL);
    ASSERT(batchOp.isFinished());

    BatchedCommandResponse clientResponse;
    batchOp.buildClientResponse(&clientResponse);
    ASSERT(clientResponse.getOk());
    ASSERT_EQUALS(clientResponse.getN(), 2);
    ASSERT(clientResponse.isErrDetailsSet());
    ASSERT_EQUALS(clientResponse.sizeErrDetails(), 1u);
    ASSERT_EQUALS(clientResponse.getErrDetailsAt(0)->getIndex(), 1);
}

// Batch failure (ok : 0) reported in a multi-op batch (ordered). Expect this gets translated down
// into write errors for first affected write.
TEST_F(BatchWriteOpTest, MultiOpFailedBatchOrdered) {
    NamespaceString nss("foo.bar");
    ShardEndpoint endpointA(ShardId("shardA"), ChunkVersion::IGNORED());
    ShardEndpoint endpointB(ShardId("shardB"), ChunkVersion::IGNORED());
    MockNSTargeter targeter;
    initTargeterSplitRange(nss, endpointA, endpointB, &targeter);

    BatchedCommandRequest request([&] {
        write_ops::Insert insertOp(nss);
        insertOp.setDocuments({BSON("x" << -1), BSON("x" << 2), BSON("x" << 3)});
        return insertOp;
    }());

    BatchWriteOp batchOp(operationContext(), request);

    OwnedPointerMap<ShardId, TargetedWriteBatch> targetedOwned;
    std::map<ShardId, TargetedWriteBatch*>& targeted = targetedOwned.mutableMap();
    ASSERT_OK(batchOp.targetBatch(targeter, false, &targeted));

    BatchedCommandResponse response;
    buildResponse(1, &response);

    // First shard batch is ok
    batchOp.noteBatchResponse(*targeted.begin()->second, response, NULL);
    ASSERT(!batchOp.isFinished());

    targetedOwned.clear();
    ASSERT_OK(batchOp.targetBatch(targeter, true, &targeted));

    buildErrResponse(ErrorCodes::UnknownError, "mock error", &response);

    // Second shard batch fails
    batchOp.noteBatchResponse(*targeted.begin()->second, response, NULL);
    ASSERT(batchOp.isFinished());

    // We should have recorded an error for the second write
    BatchedCommandResponse clientResponse;
    batchOp.buildClientResponse(&clientResponse);
    ASSERT(clientResponse.getOk());
    ASSERT_EQUALS(clientResponse.getN(), 1);
    ASSERT(clientResponse.isErrDetailsSet());
    ASSERT_EQUALS(clientResponse.sizeErrDetails(), 1u);
    ASSERT_EQUALS(clientResponse.getErrDetailsAt(0)->getIndex(), 1);
    ASSERT_EQUALS(clientResponse.getErrDetailsAt(0)->getErrCode(), response.getErrCode());
}

// Batch failure (ok : 0) reported in a multi-op batch (unordered). Expect this gets translated down
// into write errors for all affected writes.
TEST_F(BatchWriteOpTest, MultiOpFailedBatchUnordered) {
    NamespaceString nss("foo.bar");
    ShardEndpoint endpointA(ShardId("shardA"), ChunkVersion::IGNORED());
    ShardEndpoint endpointB(ShardId("shardB"), ChunkVersion::IGNORED());
    MockNSTargeter targeter;
    initTargeterSplitRange(nss, endpointA, endpointB, &targeter);

    BatchedCommandRequest request([&] {
        write_ops::Insert insertOp(nss);
        insertOp.setWriteCommandBase([] {
            write_ops::WriteCommandBase wcb;
            wcb.setOrdered(false);
            return wcb;
        }());
        insertOp.setDocuments({BSON("x" << -1), BSON("x" << 2), BSON("x" << 3)});
        return insertOp;
    }());

    BatchWriteOp batchOp(operationContext(), request);

    OwnedPointerMap<ShardId, TargetedWriteBatch> targetedOwned;
    std::map<ShardId, TargetedWriteBatch*>& targeted = targetedOwned.mutableMap();
    ASSERT_OK(batchOp.targetBatch(targeter, false, &targeted));

    // Respond to batches.
    auto targetedIt = targeted.begin();

    BatchedCommandResponse response;
    buildResponse(1, &response);

    // First shard batch is ok
    batchOp.noteBatchResponse(*targetedIt->second, response, NULL);
    ASSERT(!batchOp.isFinished());

    buildErrResponse(ErrorCodes::UnknownError, "mock error", &response);

    // Second shard batch fails
    ++targetedIt;
    batchOp.noteBatchResponse(*targetedIt->second, response, NULL);
    ASSERT(batchOp.isFinished());
    ASSERT(++targetedIt == targeted.end());

    // We should have recorded an error for the second and third write
    BatchedCommandResponse clientResponse;
    batchOp.buildClientResponse(&clientResponse);
    ASSERT(clientResponse.getOk());
    ASSERT_EQUALS(clientResponse.getN(), 1);
    ASSERT(clientResponse.isErrDetailsSet());
    ASSERT_EQUALS(clientResponse.sizeErrDetails(), 2u);
    ASSERT_EQUALS(clientResponse.getErrDetailsAt(0)->getIndex(), 1);
    ASSERT_EQUALS(clientResponse.getErrDetailsAt(0)->getErrCode(), response.getErrCode());
    ASSERT_EQUALS(clientResponse.getErrDetailsAt(1)->getIndex(), 2);
    ASSERT_EQUALS(clientResponse.getErrDetailsAt(1)->getErrCode(), response.getErrCode());
}

// Batch aborted (ordered). Expect this gets translated down into write error for first affected
// write.
TEST_F(BatchWriteOpTest, MultiOpAbortOrdered) {
    NamespaceString nss("foo.bar");
    ShardEndpoint endpointA(ShardId("shardA"), ChunkVersion::IGNORED());
    ShardEndpoint endpointB(ShardId("shardB"), ChunkVersion::IGNORED());
    MockNSTargeter targeter;
    initTargeterSplitRange(nss, endpointA, endpointB, &targeter);

    BatchedCommandRequest request([&] {
        write_ops::Insert insertOp(nss);
        insertOp.setDocuments({BSON("x" << -1), BSON("x" << 2), BSON("x" << 3)});
        return insertOp;
    }());

    BatchWriteOp batchOp(operationContext(), request);

    OwnedPointerMap<ShardId, TargetedWriteBatch> targetedOwned;
    std::map<ShardId, TargetedWriteBatch*>& targeted = targetedOwned.mutableMap();
    ASSERT_OK(batchOp.targetBatch(targeter, false, &targeted));

    BatchedCommandResponse response;
    buildResponse(1, &response);

    // First shard batch is ok
    batchOp.noteBatchResponse(*targeted.begin()->second, response, NULL);
    ASSERT(!batchOp.isFinished());

    WriteErrorDetail abortError;
    abortError.setErrCode(ErrorCodes::UnknownError);
    abortError.setErrMessage("mock abort");
    batchOp.abortBatch(abortError);
    ASSERT(batchOp.isFinished());

    // We should have recorded an error for the second write
    BatchedCommandResponse clientResponse;
    batchOp.buildClientResponse(&clientResponse);
    ASSERT(clientResponse.getOk());
    ASSERT_EQUALS(clientResponse.getN(), 1);
    ASSERT(clientResponse.isErrDetailsSet());
    ASSERT_EQUALS(clientResponse.sizeErrDetails(), 1u);
    ASSERT_EQUALS(clientResponse.getErrDetailsAt(0)->getIndex(), 1);
    ASSERT_EQUALS(clientResponse.getErrDetailsAt(0)->getErrCode(), abortError.getErrCode());
}

// Batch aborted (unordered). Expect this gets translated down into write errors for all affected
// writes.
TEST_F(BatchWriteOpTest, MultiOpAbortUnordered) {
    NamespaceString nss("foo.bar");
    ShardEndpoint endpointA(ShardId("shardA"), ChunkVersion::IGNORED());
    ShardEndpoint endpointB(ShardId("shardB"), ChunkVersion::IGNORED());
    MockNSTargeter targeter;
    initTargeterSplitRange(nss, endpointA, endpointB, &targeter);

    BatchedCommandRequest request([&] {
        write_ops::Insert insertOp(nss);
        insertOp.setWriteCommandBase([] {
            write_ops::WriteCommandBase wcb;
            wcb.setOrdered(false);
            return wcb;
        }());
        insertOp.setDocuments({BSON("x" << -1), BSON("x" << -2)});
        return insertOp;
    }());

    BatchWriteOp batchOp(operationContext(), request);

    WriteErrorDetail abortError;
    abortError.setErrCode(ErrorCodes::UnknownError);
    abortError.setErrMessage("mock abort");
    batchOp.abortBatch(abortError);
    ASSERT(batchOp.isFinished());

    // We should have recorded an error for the first and second write
    BatchedCommandResponse clientResponse;
    batchOp.buildClientResponse(&clientResponse);
    ASSERT(clientResponse.getOk());
    ASSERT_EQUALS(clientResponse.getN(), 0);
    ASSERT(clientResponse.isErrDetailsSet());
    ASSERT_EQUALS(clientResponse.sizeErrDetails(), 2u);
    ASSERT_EQUALS(clientResponse.getErrDetailsAt(0)->getIndex(), 0);
    ASSERT_EQUALS(clientResponse.getErrDetailsAt(0)->getErrCode(), abortError.getErrCode());
    ASSERT_EQUALS(clientResponse.getErrDetailsAt(1)->getIndex(), 1);
    ASSERT_EQUALS(clientResponse.getErrDetailsAt(1)->getErrCode(), abortError.getErrCode());
}

// Multi-op targeting test where each op goes to both shards and both return a write concern error
TEST_F(BatchWriteOpTest, MultiOpTwoWCErrors) {
    NamespaceString nss("foo.bar");
    ShardEndpoint endpointA(ShardId("shardA"), ChunkVersion::IGNORED());
    ShardEndpoint endpointB(ShardId("shardB"), ChunkVersion::IGNORED());
    MockNSTargeter targeter;
    initTargeterSplitRange(nss, endpointA, endpointB, &targeter);

    BatchedCommandRequest request([&] {
        write_ops::Insert insertOp(nss);
        insertOp.setDocuments({BSON("x" << -1), BSON("x" << 2)});
        return insertOp;
    }());
    request.setWriteConcern(BSON("w" << 3));

    BatchWriteOp batchOp(operationContext(), request);

    OwnedPointerMap<ShardId, TargetedWriteBatch> targetedOwned;
    std::map<ShardId, TargetedWriteBatch*>& targeted = targetedOwned.mutableMap();
    ASSERT_OK(batchOp.targetBatch(targeter, false, &targeted));

    BatchedCommandResponse response;
    buildResponse(1, &response);
    addWCError(&response);

    // First shard write write concern fails.
    batchOp.noteBatchResponse(*targeted.begin()->second, response, NULL);
    ASSERT(!batchOp.isFinished());

    targetedOwned.clear();
    ASSERT_OK(batchOp.targetBatch(targeter, true, &targeted));

    // Second shard write write concern fails.
    batchOp.noteBatchResponse(*targeted.begin()->second, response, NULL);
    ASSERT(batchOp.isFinished());

    BatchedCommandResponse clientResponse;
    batchOp.buildClientResponse(&clientResponse);
    ASSERT(clientResponse.getOk());
    ASSERT_EQUALS(clientResponse.getN(), 2);
    ASSERT(!clientResponse.isErrDetailsSet());
    ASSERT(clientResponse.isWriteConcernErrorSet());
}

//
// Tests of batch size limit functionality
//

using BatchWriteOpLimitTests = WriteOpTestFixture;

// Big single operation test - should go through
TEST_F(BatchWriteOpLimitTests, OneBigDoc) {
    NamespaceString nss("foo.bar");
    ShardEndpoint endpoint(ShardId("shard"), ChunkVersion::IGNORED());
    MockNSTargeter targeter;
    initTargeterFullRange(nss, endpoint, &targeter);

    // Create a BSONObj (slightly) bigger than the maximum size by including a max-size string
    const std::string bigString(BSONObjMaxUserSize, 'x');

    // Do single-target, single doc batch write op
    BatchedCommandRequest request([&] {
        write_ops::Insert insertOp(nss);
        insertOp.setWriteCommandBase([] {
            write_ops::WriteCommandBase wcb;
            wcb.setOrdered(false);
            return wcb;
        }());
        insertOp.setDocuments({BSON("x" << 1 << "data" << bigString)});
        return insertOp;
    }());

    BatchWriteOp batchOp(operationContext(), request);

    OwnedPointerMap<ShardId, TargetedWriteBatch> targetedOwned;
    std::map<ShardId, TargetedWriteBatch*>& targeted = targetedOwned.mutableMap();
    ASSERT_OK(batchOp.targetBatch(targeter, false, &targeted));
    ASSERT_EQUALS(targeted.size(), 1u);

    BatchedCommandResponse response;
    buildResponse(1, &response);

    batchOp.noteBatchResponse(*targeted.begin()->second, response, NULL);
    ASSERT(batchOp.isFinished());
}

// Big doc with smaller additional doc - should go through as two batches
TEST_F(BatchWriteOpLimitTests, OneBigOneSmall) {
    NamespaceString nss("foo.bar");
    ShardEndpoint endpoint(ShardId("shard"), ChunkVersion::IGNORED());
    MockNSTargeter targeter;
    initTargeterFullRange(nss, endpoint, &targeter);

    // Create a BSONObj (slightly) bigger than the maximum size by including a max-size string
    const std::string bigString(BSONObjMaxUserSize, 'x');

    BatchedCommandRequest request([&] {
        write_ops::Update updateOp(nss);
        updateOp.setUpdates({buildUpdate(BSON("x" << 1), BSON("data" << bigString), false),
                             buildUpdate(BSON("x" << 2), BSONObj(), false)});
        return updateOp;
    }());

    BatchWriteOp batchOp(operationContext(), request);

    OwnedPointerMap<ShardId, TargetedWriteBatch> targetedOwned;
    std::map<ShardId, TargetedWriteBatch*>& targeted = targetedOwned.mutableMap();
    ASSERT_OK(batchOp.targetBatch(targeter, false, &targeted));
    ASSERT_EQUALS(targeted.size(), 1u);
    ASSERT_EQUALS(targeted.begin()->second->getWrites().size(), 1u);

    BatchedCommandResponse response;
    buildResponse(1, &response);

    batchOp.noteBatchResponse(*targeted.begin()->second, response, NULL);
    ASSERT(!batchOp.isFinished());

    targetedOwned.clear();
    ASSERT_OK(batchOp.targetBatch(targeter, false, &targeted));
    ASSERT_EQUALS(targeted.size(), 1u);
    ASSERT_EQUALS(targeted.begin()->second->getWrites().size(), 1u);

    batchOp.noteBatchResponse(*targeted.begin()->second, response, NULL);
    ASSERT(batchOp.isFinished());
}

}  // namespace
}  // namespace mongo
