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

#include "mongo/db/timeseries/bucket_catalog/bucket_state_registry.h"
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
// IWYU pragma: no_include "ext/alloc_traits.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/ops/write_ops_gen.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/db/session/logical_session_id_helpers.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/catalog_cache_test_fixture.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/collection_routing_info_targeter.h"
#include "mongo/s/database_version.h"
#include "mongo/s/index_version.h"
#include "mongo/s/mock_ns_targeter.h"
#include "mongo/s/session_catalog_router.h"
#include "mongo/s/shard_cannot_refresh_due_to_locks_held_exception.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/s/shard_version.h"
#include "mongo/s/shard_version_factory.h"
#include "mongo/s/sharding_mongos_test_fixture.h"
#include "mongo/s/stale_exception.h"
#include "mongo/s/transaction_router.h"
#include "mongo/s/write_ops/batch_write_op.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/fail_point.h"

namespace mongo {
namespace {

const NamespaceString kNss = NamespaceString::createNamespaceString_forTest("TestDB", "TestColl");
const int splitPoint = 0;

auto initTargeterFullRange(const NamespaceString& nss, const ShardEndpoint& endpoint) {
    return MockNSTargeter(nss, {MockRange(endpoint, BSON("x" << MINKEY), BSON("x" << MAXKEY))});
}

auto initTargeterSplitRange(const NamespaceString& nss,
                            const ShardEndpoint& endpointA,
                            const ShardEndpoint& endpointB) {
    return MockNSTargeter(nss,
                          {MockRange(endpointA, BSON("x" << MINKEY), BSON("x" << 0)),
                           MockRange(endpointB, BSON("x" << 0), BSON("x" << MAXKEY))});
}

auto initTargeterHalfRange(const NamespaceString& nss, const ShardEndpoint& endpoint) {
    // x >= 0 values are untargetable
    return MockNSTargeter(nss, {MockRange(endpoint, BSON("x" << MINKEY), BSON("x" << 0))});
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
    entry.setU(write_ops::UpdateModification::parseFromClassicUpdate(BSONObj()));
    entry.setMulti(multi);
    return entry;
}

write_ops::UpdateOpEntry buildUpdate(const BSONObj& query, const BSONObj& updateExpr, bool multi) {
    write_ops::UpdateOpEntry entry;
    entry.setQ(query);
    entry.setU(write_ops::UpdateModification::parseFromClassicUpdate(updateExpr));
    entry.setMulti(multi);
    return entry;
}

void buildResponse(int n, BatchedCommandResponse* response) {
    response->clear();
    response->setStatus(Status::OK());
    response->setN(n);
}

void buildErrResponse(int code, const std::string& message, BatchedCommandResponse* response) {
    response->clear();
    response->setN(0);
    response->setStatus({ErrorCodes::Error(code), message});
}

void addError(int code, const std::string& message, int index, BatchedCommandResponse* response) {
    response->addToErrDetails(write_ops::WriteError(index, {ErrorCodes::Error(code), message}));
}

void addWCError(BatchedCommandResponse* response) {
    std::unique_ptr<WriteConcernErrorDetail> error(new WriteConcernErrorDetail);
    error->setStatus({ErrorCodes::WriteConcernFailed, "mock wc error"});

    response->setWriteConcernError(error.release());
}

class WriteOpTestFixture : public ServiceContextTest {
protected:
    WriteOpTestFixture() {
        _opCtxHolder = makeOperationContext();
        _opCtx = _opCtxHolder.get();
    }

    ServiceContext::UniqueOperationContext _opCtxHolder;
    OperationContext* _opCtx;

    // This failpoint is to skip running the useTwoPhaseWriteProtocol check which expects the Grid
    // to be initialized. With the feature flag on, the helper always returns false, which signifies
    // that we have a targetable write op.
    std::unique_ptr<FailPointEnableBlock> _skipUseTwoPhaseWriteProtocolCheck =
        std::make_unique<FailPointEnableBlock>("skipUseTwoPhaseWriteProtocolCheck");
};

using BatchWriteOpTest = WriteOpTestFixture;

// For BatchWriteOp, all writes in the batch should share the same endpoint since they
// target the same shard and namespace. So we just use the endpoint from the first write.
const ShardEndpoint& getFirstTargetedWriteEndpoint(
    const std::unique_ptr<TargetedWriteBatch>& targetedBatch) {
    return targetedBatch->getWrites()[0]->endpoint;
}

TEST_F(BatchWriteOpTest, SingleOp) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("foo.bar");
    ShardEndpoint endpoint(ShardId("shard"),
                           ShardVersionFactory::make(ChunkVersion::IGNORED(), boost::none),
                           boost::none);

    auto targeter = initTargeterFullRange(nss, endpoint);

    // Do single-target, single doc batch write op
    BatchedCommandRequest request([&] {
        write_ops::InsertCommandRequest insertOp(nss);
        insertOp.setDocuments({BSON("x" << 1)});
        return insertOp;
    }());

    BatchWriteOp batchOp(_opCtx, request);

    std::map<ShardId, std::unique_ptr<TargetedWriteBatch>> targeted;
    ASSERT_OK(batchOp.targetBatch(targeter, false, &targeted));
    ASSERT(!batchOp.isFinished());
    ASSERT_EQUALS(targeted.size(), 1u);
    assertEndpointsEqual(getFirstTargetedWriteEndpoint(targeted.begin()->second), endpoint);

    BatchedCommandResponse response;
    buildResponse(1, &response);

    batchOp.noteBatchResponse(*targeted.begin()->second, response, nullptr);
    ASSERT(batchOp.isFinished());

    BatchedCommandResponse clientResponse;
    batchOp.buildClientResponse(&clientResponse);
    ASSERT(clientResponse.getOk());
}

TEST_F(BatchWriteOpTest, SingleError) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("foo.bar");
    ShardEndpoint endpoint(ShardId("shard"),
                           ShardVersionFactory::make(ChunkVersion::IGNORED(), boost::none),
                           boost::none);

    auto targeter = initTargeterFullRange(nss, endpoint);

    // Do single-target, single doc batch write op
    BatchedCommandRequest request([&] {
        write_ops::DeleteCommandRequest deleteOp(nss);
        deleteOp.setDeletes({buildDelete(BSON("x" << 1), false)});
        return deleteOp;
    }());

    BatchWriteOp batchOp(_opCtx, request);

    std::map<ShardId, std::unique_ptr<TargetedWriteBatch>> targeted;
    ASSERT_OK(batchOp.targetBatch(targeter, false, &targeted));
    ASSERT(!batchOp.isFinished());
    ASSERT_EQUALS(targeted.size(), 1u);
    assertEndpointsEqual(getFirstTargetedWriteEndpoint(targeted.begin()->second), endpoint);

    BatchedCommandResponse response;
    buildErrResponse(ErrorCodes::UnknownError, "message", &response);

    batchOp.noteBatchResponse(*targeted.begin()->second, response, nullptr);
    ASSERT(batchOp.isFinished());

    BatchedCommandResponse clientResponse;
    batchOp.buildClientResponse(&clientResponse);

    ASSERT(clientResponse.getOk());
    ASSERT_EQUALS(clientResponse.sizeErrDetails(), 1u);
    ASSERT_EQUALS(clientResponse.getErrDetailsAt(0).getStatus().code(), response.toStatus().code());
    ASSERT(clientResponse.getErrDetailsAt(0).getStatus().reason().find(
               response.toStatus().reason()) != std::string::npos);
    ASSERT_EQUALS(clientResponse.getN(), 0);
}

TEST_F(BatchWriteOpTest, SingleTargetError) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("foo.bar");
    ShardEndpoint endpoint(ShardId("shard"),
                           ShardVersionFactory::make(ChunkVersion::IGNORED(), boost::none),
                           boost::none);

    auto targeter = initTargeterHalfRange(nss, endpoint);

    // Do untargetable delete op
    BatchedCommandRequest request([&] {
        write_ops::DeleteCommandRequest deleteOp(nss);
        deleteOp.setDeletes({buildDelete(BSON("x" << 1), false)});
        return deleteOp;
    }());

    BatchWriteOp batchOp(_opCtx, request);

    std::map<ShardId, std::unique_ptr<TargetedWriteBatch>> targeted;
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
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("foo.bar");
    ShardEndpoint endpoint(ShardId("shard"),
                           ShardVersionFactory::make(ChunkVersion::IGNORED(), boost::none),
                           boost::none);

    auto targeter = initTargeterFullRange(nss, endpoint);

    BatchedCommandRequest request([&] {
        write_ops::InsertCommandRequest insertOp(nss);
        insertOp.setDocuments({BSON("x" << 1)});
        return insertOp;
    }());
    _opCtx->setWriteConcern(WriteConcernOptions::parse(BSON("w" << 3)).getValue());

    BatchWriteOp batchOp(_opCtx, request);

    std::map<ShardId, std::unique_ptr<TargetedWriteBatch>> targeted;
    ASSERT_OK(batchOp.targetBatch(targeter, false, &targeted));
    ASSERT(!batchOp.isFinished());
    ASSERT_EQUALS(targeted.size(), 1u);
    assertEndpointsEqual(getFirstTargetedWriteEndpoint(targeted.begin()->second), endpoint);

    BatchedCommandRequest targetBatch =
        batchOp.buildBatchRequest(*targeted.begin()->second, targeter, boost::none);
    ASSERT(targetBatch.getWriteConcern().woCompare(_opCtx->getWriteConcern().toBSON()) == 0);

    BatchedCommandResponse response;
    buildResponse(1, &response);
    addWCError(&response);

    // First stale response comes back, we should retry
    batchOp.noteBatchResponse(*targeted.begin()->second, response, nullptr);
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
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("foo.bar");
    ShardEndpoint endpoint(ShardId("shard"),
                           ShardVersionFactory::make(ChunkVersion::IGNORED(), boost::none),
                           boost::none);

    auto targeter = initTargeterFullRange(nss, endpoint);

    BatchedCommandRequest request([&] {
        write_ops::InsertCommandRequest insertOp(nss);
        insertOp.setDocuments({BSON("x" << 1)});
        return insertOp;
    }());

    BatchWriteOp batchOp(_opCtx, request);

    std::map<ShardId, std::unique_ptr<TargetedWriteBatch>> targeted;
    ASSERT_OK(batchOp.targetBatch(targeter, false, &targeted));

    BatchedCommandResponse response;
    buildResponse(0, &response);
    OID epoch{OID::gen()};
    Timestamp timestamp{1, 0};
    response.addToErrDetails(write_ops::WriteError(
        0,
        Status{StaleConfigInfo(
                   nss,
                   ShardVersionFactory::make(ChunkVersion({epoch, timestamp}, {101, 200}),
                                             boost::optional<CollectionIndexes>(boost::none)),
                   ShardVersionFactory::make(ChunkVersion({epoch, timestamp}, {105, 200}),
                                             boost::optional<CollectionIndexes>(boost::none)),
                   ShardId("shard")),
               "mock stale error"}));

    // First stale response comes back, we should retry
    batchOp.noteBatchResponse(*targeted.begin()->second, response, nullptr);
    ASSERT(!batchOp.isFinished());

    targeted.clear();
    ASSERT_OK(batchOp.targetBatch(targeter, false, &targeted));

    // Respond again with a stale response
    batchOp.noteBatchResponse(*targeted.begin()->second, response, nullptr);
    ASSERT(!batchOp.isFinished());

    // Respond with a ShardCannotRefreshDueToLocksHeld error; the batch should still be retriable.
    targeted.clear();
    ASSERT_OK(batchOp.targetBatch(targeter, false, &targeted));
    buildResponse(0, &response);
    response.addToErrDetails(write_ops::WriteError(
        0, Status{ShardCannotRefreshDueToLocksHeldInfo(nss), "mock cache busy error"}));

    batchOp.noteBatchResponse(*targeted.begin()->second, response, nullptr);
    ASSERT(!batchOp.isFinished());

    // Respond with an 'ok' response
    targeted.clear();
    ASSERT_OK(batchOp.targetBatch(targeter, false, &targeted));

    buildResponse(1, &response);

    batchOp.noteBatchResponse(*targeted.begin()->second, response, nullptr);
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
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("foo.bar");
    ShardEndpoint endpoint(ShardId("shard"),
                           ShardVersionFactory::make(ChunkVersion::IGNORED(), boost::none),
                           boost::none);

    auto targeter = initTargeterFullRange(nss, endpoint);

    // Do single-target, multi-doc batch write op
    BatchedCommandRequest request([&] {
        write_ops::UpdateCommandRequest updateOp(nss);
        updateOp.setUpdates(
            {buildUpdate(BSON("x" << 1), false), buildUpdate(BSON("x" << 2), false)});
        return updateOp;
    }());

    BatchWriteOp batchOp(_opCtx, request);

    std::map<ShardId, std::unique_ptr<TargetedWriteBatch>> targeted;
    ASSERT_OK(batchOp.targetBatch(targeter, false, &targeted));
    ASSERT(!batchOp.isFinished());
    ASSERT_EQUALS(targeted.size(), 1u);
    ASSERT_EQUALS(targeted.begin()->second->getWrites().size(), 2u);
    assertEndpointsEqual(getFirstTargetedWriteEndpoint(targeted.begin()->second), endpoint);

    BatchedCommandResponse response;
    buildResponse(2, &response);

    batchOp.noteBatchResponse(*targeted.begin()->second, response, nullptr);
    ASSERT(batchOp.isFinished());

    BatchedCommandResponse clientResponse;
    batchOp.buildClientResponse(&clientResponse);
    ASSERT(clientResponse.getOk());
    ASSERT_EQUALS(clientResponse.getN(), 2);
}

// Multi-op targeting test (unordered)
TEST_F(BatchWriteOpTest, MultiOpSameShardUnordered) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("foo.bar");
    ShardEndpoint endpoint(ShardId("shard"),
                           ShardVersionFactory::make(ChunkVersion::IGNORED(), boost::none),
                           boost::none);

    auto targeter = initTargeterFullRange(nss, endpoint);

    // Do single-target, multi-doc batch write op
    BatchedCommandRequest request([&] {
        write_ops::UpdateCommandRequest updateOp(nss);
        updateOp.setWriteCommandRequestBase([] {
            write_ops::WriteCommandRequestBase wcb;
            wcb.setOrdered(false);
            return wcb;
        }());
        updateOp.setUpdates(
            {buildUpdate(BSON("x" << 1), false), buildUpdate(BSON("x" << 2), false)});
        return updateOp;
    }());

    BatchWriteOp batchOp(_opCtx, request);

    std::map<ShardId, std::unique_ptr<TargetedWriteBatch>> targeted;
    ASSERT_OK(batchOp.targetBatch(targeter, false, &targeted));
    ASSERT(!batchOp.isFinished());
    ASSERT_EQUALS(targeted.size(), 1u);
    ASSERT_EQUALS(targeted.begin()->second->getWrites().size(), 2u);
    assertEndpointsEqual(getFirstTargetedWriteEndpoint(targeted.begin()->second), endpoint);

    BatchedCommandResponse response;
    buildResponse(2, &response);

    batchOp.noteBatchResponse(*targeted.begin()->second, response, nullptr);
    ASSERT(batchOp.isFinished());

    BatchedCommandResponse clientResponse;
    batchOp.buildClientResponse(&clientResponse);
    ASSERT(clientResponse.getOk());
    ASSERT_EQUALS(clientResponse.getN(), 2);
}

// Multi-op, multi-endpoing targeting test (ordered). There should be two sets of single batches
// (one to each shard, one-by-one)
TEST_F(BatchWriteOpTest, MultiOpTwoShardsOrdered) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("foo.bar");
    ShardEndpoint endpointA(ShardId("shardA"),
                            ShardVersionFactory::make(ChunkVersion::IGNORED(), boost::none),
                            boost::none);
    ShardEndpoint endpointB(ShardId("shardB"),
                            ShardVersionFactory::make(ChunkVersion::IGNORED(), boost::none),
                            boost::none);

    auto targeter = initTargeterSplitRange(nss, endpointA, endpointB);

    // Do multi-target, multi-doc batch write op
    BatchedCommandRequest request([&] {
        write_ops::InsertCommandRequest insertOp(nss);
        insertOp.setDocuments({BSON("x" << -1), BSON("x" << 1)});
        return insertOp;
    }());

    BatchWriteOp batchOp(_opCtx, request);

    std::map<ShardId, std::unique_ptr<TargetedWriteBatch>> targeted;
    ASSERT_OK(batchOp.targetBatch(targeter, false, &targeted));
    ASSERT(!batchOp.isFinished());
    ASSERT_EQUALS(targeted.size(), 1u);
    ASSERT_EQUALS(targeted.begin()->second->getWrites().size(), 1u);
    assertEndpointsEqual(getFirstTargetedWriteEndpoint(targeted.begin()->second), endpointA);

    BatchedCommandResponse response;
    buildResponse(1, &response);

    // Respond to first targeted batch
    batchOp.noteBatchResponse(*targeted.begin()->second, response, nullptr);
    ASSERT(!batchOp.isFinished());

    targeted.clear();

    ASSERT_OK(batchOp.targetBatch(targeter, false, &targeted));
    ASSERT(!batchOp.isFinished());
    ASSERT_EQUALS(targeted.size(), 1u);
    ASSERT_EQUALS(targeted.begin()->second->getWrites().size(), 1u);
    assertEndpointsEqual(getFirstTargetedWriteEndpoint(targeted.begin()->second), endpointB);

    // Respond to second targeted batch
    batchOp.noteBatchResponse(*targeted.begin()->second, response, nullptr);
    ASSERT(batchOp.isFinished());

    BatchedCommandResponse clientResponse;
    batchOp.buildClientResponse(&clientResponse);
    ASSERT(clientResponse.getOk());
    ASSERT_EQUALS(clientResponse.getN(), 2);
}

void verifyTargetedBatches(std::map<ShardId, size_t> expected,
                           const std::map<ShardId, std::unique_ptr<TargetedWriteBatch>>& targeted) {
    // 'expected' contains each ShardId that was expected to be targeted and the size of the batch
    // that was expected to be targeted to it.
    // We check that each ShardId in 'targeted' corresponds to one in 'expected', in that it
    // contains a batch of the correct size.
    // Finally, we ensure that no additional ShardIds are present in 'targeted' than 'expected'.
    for (auto it = targeted.begin(); it != targeted.end(); ++it) {
        ASSERT_EQUALS(expected[getFirstTargetedWriteEndpoint(it->second).shardName],
                      it->second->getWrites().size());
        ASSERT_EQUALS(ShardVersionFactory::make(ChunkVersion::IGNORED(), boost::none),
                      *getFirstTargetedWriteEndpoint(it->second).shardVersion);
        expected.erase(expected.find(getFirstTargetedWriteEndpoint(it->second).shardName));
    }
    ASSERT(expected.empty());
}

// Multi-op, multi-endpoint targeting test (unordered). There should be one set of two batches (one
// to each shard).
TEST_F(BatchWriteOpTest, MultiOpTwoShardsUnordered) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("foo.bar");
    ShardEndpoint endpointA(ShardId("shardA"),
                            ShardVersionFactory::make(ChunkVersion::IGNORED(), boost::none),
                            boost::none);
    ShardEndpoint endpointB(ShardId("shardB"),
                            ShardVersionFactory::make(ChunkVersion::IGNORED(), boost::none),
                            boost::none);

    auto targeter = initTargeterSplitRange(nss, endpointA, endpointB);

    // Do multi-target, multi-doc batch write op
    BatchedCommandRequest request([&] {
        write_ops::InsertCommandRequest insertOp(nss);
        insertOp.setWriteCommandRequestBase([] {
            write_ops::WriteCommandRequestBase wcb;
            wcb.setOrdered(false);
            return wcb;
        }());
        insertOp.setDocuments({BSON("x" << -1), BSON("x" << 1)});
        return insertOp;
    }());

    BatchWriteOp batchOp(_opCtx, request);

    std::map<ShardId, std::unique_ptr<TargetedWriteBatch>> targeted;
    ASSERT_OK(batchOp.targetBatch(targeter, false, &targeted));
    ASSERT(!batchOp.isFinished());
    ASSERT_EQUALS(targeted.size(), 2u);
    verifyTargetedBatches({{endpointA.shardName, 1u}, {endpointB.shardName, 1u}}, targeted);

    BatchedCommandResponse response;
    buildResponse(1, &response);

    // Respond to both targeted batches
    for (auto it = targeted.begin(); it != targeted.end(); ++it) {
        ASSERT(!batchOp.isFinished());
        batchOp.noteBatchResponse(*it->second, response, nullptr);
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
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("foo.bar");
    ShardEndpoint endpointA(ShardId("shardA"),
                            ShardVersionFactory::make(ChunkVersion::IGNORED(), boost::none),
                            boost::none);
    ShardEndpoint endpointB(ShardId("shardB"),
                            ShardVersionFactory::make(ChunkVersion::IGNORED(), boost::none),
                            boost::none);

    auto targeter = initTargeterSplitRange(nss, endpointA, endpointB);

    // Do multi-target, multi-doc batch write op
    BatchedCommandRequest request([&] {
        write_ops::DeleteCommandRequest deleteOp(nss);
        deleteOp.setDeletes({buildDelete(BSON("x" << GTE << -1 << LT << 2), true),
                             buildDelete(BSON("x" << GTE << -2 << LT << 1), true)});
        return deleteOp;
    }());

    BatchWriteOp batchOp(_opCtx, request);

    std::map<ShardId, std::unique_ptr<TargetedWriteBatch>> targeted;
    ASSERT_OK(batchOp.targetBatch(targeter, false, &targeted));
    ASSERT(!batchOp.isFinished());
    ASSERT_EQUALS(targeted.size(), 2u);
    verifyTargetedBatches({{endpointA.shardName, 1u}, {endpointB.shardName, 1u}}, targeted);

    BatchedCommandResponse response;
    buildResponse(1, &response);

    // Respond to both targeted batches for first multi-delete
    for (auto it = targeted.begin(); it != targeted.end(); ++it) {
        ASSERT(!batchOp.isFinished());
        batchOp.noteBatchResponse(*it->second, response, nullptr);
    }
    ASSERT(!batchOp.isFinished());

    targeted.clear();

    ASSERT_OK(batchOp.targetBatch(targeter, false, &targeted));
    ASSERT(!batchOp.isFinished());
    ASSERT_EQUALS(targeted.size(), 2u);
    verifyTargetedBatches({{endpointA.shardName, 1u}, {endpointB.shardName, 1u}}, targeted);

    // Respond to second targeted batches for second multi-delete
    for (auto it = targeted.begin(); it != targeted.end(); ++it) {
        ASSERT(!batchOp.isFinished());
        batchOp.noteBatchResponse(*it->second, response, nullptr);
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
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("foo.bar");
    ShardEndpoint endpointA(ShardId("shardA"),
                            ShardVersionFactory::make(ChunkVersion::IGNORED(), boost::none),
                            boost::none);
    ShardEndpoint endpointB(ShardId("shardB"),
                            ShardVersionFactory::make(ChunkVersion::IGNORED(), boost::none),
                            boost::none);

    auto targeter = initTargeterSplitRange(nss, endpointA, endpointB);

    // Do multi-target, multi-doc batch write op
    BatchedCommandRequest request([&] {
        write_ops::UpdateCommandRequest updateOp(nss);
        updateOp.setWriteCommandRequestBase([] {
            write_ops::WriteCommandRequestBase wcb;
            wcb.setOrdered(false);
            return wcb;
        }());
        updateOp.setUpdates({buildUpdate(BSON("x" << GTE << -1 << LT << 2), true),
                             buildUpdate(BSON("x" << GTE << -2 << LT << 1), true)});
        return updateOp;
    }());

    BatchWriteOp batchOp(_opCtx, request);

    std::map<ShardId, std::unique_ptr<TargetedWriteBatch>> targeted;
    ASSERT_OK(batchOp.targetBatch(targeter, false, &targeted));
    ASSERT(!batchOp.isFinished());
    ASSERT_EQUALS(targeted.size(), 2u);
    verifyTargetedBatches({{endpointA.shardName, 2u}, {endpointB.shardName, 2u}}, targeted);

    BatchedCommandResponse response;
    buildResponse(2, &response);

    // Respond to both targeted batches, each containing two ops
    for (auto it = targeted.begin(); it != targeted.end(); ++it) {
        ASSERT(!batchOp.isFinished());
        batchOp.noteBatchResponse(*it->second, response, nullptr);
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
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("foo.bar");
    ShardEndpoint endpointA(ShardId("shardA"),
                            ShardVersionFactory::make(ChunkVersion::IGNORED(), boost::none),
                            boost::none);
    ShardEndpoint endpointB(ShardId("shardB"),
                            ShardVersionFactory::make(ChunkVersion::IGNORED(), boost::none),
                            boost::none);

    auto targeter = initTargeterSplitRange(nss, endpointA, endpointB);

    BatchedCommandRequest request([&] {
        write_ops::DeleteCommandRequest deleteOp(nss);
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

    BatchWriteOp batchOp(_opCtx, request);

    std::map<ShardId, std::unique_ptr<TargetedWriteBatch>> targeted;
    ASSERT_OK(batchOp.targetBatch(targeter, false, &targeted));
    ASSERT(!batchOp.isFinished());
    ASSERT_EQUALS(targeted.size(), 1u);
    ASSERT_EQUALS(targeted.begin()->second->getWrites().size(), 2u);
    assertEndpointsEqual(getFirstTargetedWriteEndpoint(targeted.begin()->second), endpointA);

    BatchedCommandResponse response;
    // Emulate one-write-per-delete-per-host
    buildResponse(2, &response);

    // Respond to first targeted batch containing the two single-host deletes
    batchOp.noteBatchResponse(*targeted.begin()->second, response, nullptr);
    ASSERT(!batchOp.isFinished());

    targeted.clear();

    ASSERT_OK(batchOp.targetBatch(targeter, false, &targeted));
    ASSERT(!batchOp.isFinished());
    ASSERT_EQUALS(targeted.size(), 2u);
    verifyTargetedBatches({{endpointA.shardName, 1u}, {endpointB.shardName, 1u}}, targeted);

    // Emulate one-write-per-delete-per-host
    buildResponse(1, &response);

    // Respond to two targeted batches for first multi-delete
    for (auto it = targeted.begin(); it != targeted.end(); ++it) {
        ASSERT(!batchOp.isFinished());
        batchOp.noteBatchResponse(*it->second, response, nullptr);
    }
    ASSERT(!batchOp.isFinished());

    targeted.clear();

    ASSERT_OK(batchOp.targetBatch(targeter, false, &targeted));
    ASSERT(!batchOp.isFinished());
    ASSERT_EQUALS(targeted.size(), 2u);
    verifyTargetedBatches({{endpointA.shardName, 1u}, {endpointB.shardName, 1u}}, targeted);

    // Respond to two targeted batches for second multi-delete
    for (auto it = targeted.begin(); it != targeted.end(); ++it) {
        ASSERT(!batchOp.isFinished());
        batchOp.noteBatchResponse(*it->second, response, nullptr);
    }
    ASSERT(!batchOp.isFinished());

    targeted.clear();

    ASSERT_OK(batchOp.targetBatch(targeter, false, &targeted));
    ASSERT(!batchOp.isFinished());
    ASSERT_EQUALS(targeted.size(), 1u);
    ASSERT_EQUALS(targeted.begin()->second->getWrites().size(), 2u);
    assertEndpointsEqual(getFirstTargetedWriteEndpoint(targeted.begin()->second), endpointB);

    // Emulate one-write-per-delete-per-host
    buildResponse(2, &response);

    // Respond to final targeted batch containing the last two single-host deletes
    batchOp.noteBatchResponse(*targeted.begin()->second, response, nullptr);
    ASSERT(batchOp.isFinished());

    BatchedCommandResponse clientResponse;
    batchOp.buildClientResponse(&clientResponse);
    ASSERT(clientResponse.getOk());
    ASSERT_EQUALS(clientResponse.getN(), 8);
}

// Multi-op (unordered) targeting test where first two ops go to one shard, second two ops go to two
// shards. Should batch all the ops together into two batches of four ops for each shard.
TEST_F(BatchWriteOpTest, MultiOpOneOrTwoShardsUnordered) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("foo.bar");
    ShardEndpoint endpointA(ShardId("shardA"),
                            ShardVersionFactory::make(ChunkVersion::IGNORED(), boost::none),
                            boost::none);
    ShardEndpoint endpointB(ShardId("shardB"),
                            ShardVersionFactory::make(ChunkVersion::IGNORED(), boost::none),
                            boost::none);

    auto targeter = initTargeterSplitRange(nss, endpointA, endpointB);

    BatchedCommandRequest request([&] {
        write_ops::UpdateCommandRequest updateOp(nss);
        updateOp.setWriteCommandRequestBase([] {
            write_ops::WriteCommandRequestBase wcb;
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

    BatchWriteOp batchOp(_opCtx, request);

    std::map<ShardId, std::unique_ptr<TargetedWriteBatch>> targeted;
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
        batchOp.noteBatchResponse(*it->second, response, nullptr);
    }
    ASSERT(batchOp.isFinished());

    BatchedCommandResponse clientResponse;
    batchOp.buildClientResponse(&clientResponse);
    ASSERT(clientResponse.getOk());
    ASSERT_EQUALS(clientResponse.getN(), 8);
}

// Multi-op (unordered) targeting test where first two ops go to one shard, second two ops go to two
// shards and the last two ops go to the other shard. Multi-target ops will override the endpoint to
// have ignored placement version. If a shard is already targeted with a different shardVersion, a
// new batch is required. So this test should result in three batches:
// 1. shardA: [op1, op2]
// 2. shardA: [op3, op4], shardB: [op3, op4]
// 3. shardB: [op5, op6]
TEST_F(BatchWriteOpTest, MultiOpOneOrTwoShardsUnorderedWithDifferentEndpoints) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("foo.bar");
    ShardId shardIdA("shardA");
    ShardId shardIdB("shardB");
    ShardEndpoint endpointA(
        shardIdA,
        ShardVersionFactory::make(ChunkVersion({OID::gen(), Timestamp(2)}, {10, 11}),
                                  boost::optional<CollectionIndexes>(boost::none)),
        boost::none);
    ShardEndpoint endpointB(
        shardIdB,
        ShardVersionFactory::make(ChunkVersion({OID::gen(), Timestamp(2)}, {10, 11}),
                                  boost::optional<CollectionIndexes>(boost::none)),
        boost::none);
    ShardEndpoint endpointAVersionIgnored(
        shardIdA, ShardVersionFactory::make(ChunkVersion::IGNORED(), boost::none), boost::none);
    ShardEndpoint endpointBVersionIgnored(
        shardIdB, ShardVersionFactory::make(ChunkVersion::IGNORED(), boost::none), boost::none);

    auto targeter = initTargeterSplitRange(nss, endpointA, endpointB);

    BatchedCommandRequest request([&] {
        write_ops::UpdateCommandRequest updateOp(nss);
        updateOp.setWriteCommandRequestBase([] {
            write_ops::WriteCommandRequestBase wcb;
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

    BatchWriteOp batchOp(_opCtx, request);

    TargetedBatchMap targeted;
    // First batch: shardA: [op1, op2].
    ASSERT_OK(batchOp.targetBatch(targeter, false, &targeted));
    ASSERT_EQUALS(targeted.size(), 1u);
    ASSERT_EQUALS(targeted[shardIdA]->getWrites().size(), 2u);
    assertEndpointsEqual(targeted[shardIdA]->getWrites()[0]->endpoint, endpointA);
    assertEndpointsEqual(targeted[shardIdA]->getWrites()[1]->endpoint, endpointA);

    targeted.clear();
    // Second batch: shardA: [op3, op4], shardB: [op3, op4].
    ASSERT_OK(batchOp.targetBatch(targeter, false, &targeted));
    ASSERT_EQUALS(targeted.size(), 2u);
    ASSERT_EQUALS(targeted[shardIdA]->getWrites().size(), 2u);
    assertEndpointsEqual(targeted[shardIdA]->getWrites()[0]->endpoint, endpointAVersionIgnored);
    assertEndpointsEqual(targeted[shardIdA]->getWrites()[1]->endpoint, endpointAVersionIgnored);
    ASSERT_EQUALS(targeted[shardIdB]->getWrites().size(), 2u);
    assertEndpointsEqual(targeted[shardIdB]->getWrites()[0]->endpoint, endpointBVersionIgnored);
    assertEndpointsEqual(targeted[shardIdB]->getWrites()[1]->endpoint, endpointBVersionIgnored);

    targeted.clear();
    // Third batch: shardB: [op5, op6].
    ASSERT_OK(batchOp.targetBatch(targeter, false, &targeted));
    ASSERT_EQUALS(targeted.size(), 1u);
    ASSERT_EQUALS(targeted[shardIdB]->getWrites().size(), 2u);
    assertEndpointsEqual(targeted[shardIdB]->getWrites()[0]->endpoint, endpointB);
    assertEndpointsEqual(targeted[shardIdB]->getWrites()[1]->endpoint, endpointB);
}

// Multi-op targeting test where two ops go to two separate shards and there's an error on one op on
// one shard. There should be one set of two batches to each shard and an error reported.
TEST_F(BatchWriteOpTest, MultiOpSingleShardErrorUnordered) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("foo.bar");
    ShardEndpoint endpointA(ShardId("shardA"),
                            ShardVersionFactory::make(ChunkVersion::IGNORED(), boost::none),
                            boost::none);
    ShardEndpoint endpointB(ShardId("shardB"),
                            ShardVersionFactory::make(ChunkVersion::IGNORED(), boost::none),
                            boost::none);

    auto targeter = initTargeterSplitRange(nss, endpointA, endpointB);

    BatchedCommandRequest request([&] {
        write_ops::InsertCommandRequest insertOp(nss);
        insertOp.setWriteCommandRequestBase([] {
            write_ops::WriteCommandRequestBase wcb;
            wcb.setOrdered(false);
            return wcb;
        }());
        insertOp.setDocuments({BSON("x" << -1), BSON("x" << 1)});
        return insertOp;
    }());

    BatchWriteOp batchOp(_opCtx, request);

    std::map<ShardId, std::unique_ptr<TargetedWriteBatch>> targeted;
    ASSERT_OK(batchOp.targetBatch(targeter, false, &targeted));
    ASSERT(!batchOp.isFinished());
    ASSERT_EQUALS(targeted.size(), 2u);
    verifyTargetedBatches({{endpointA.shardName, 1u}, {endpointB.shardName, 1u}}, targeted);

    BatchedCommandResponse response;
    buildResponse(1, &response);

    // Respond to batches.
    auto targetedIt = targeted.begin();

    // No error on first shard
    batchOp.noteBatchResponse(*targetedIt->second, response, nullptr);
    ASSERT(!batchOp.isFinished());

    buildResponse(0, &response);
    addError(ErrorCodes::UnknownError, "mock error", 0, &response);

    // Error on second write on second shard
    ++targetedIt;
    batchOp.noteBatchResponse(*targetedIt->second, response, nullptr);
    ASSERT(batchOp.isFinished());
    ASSERT(++targetedIt == targeted.end());

    BatchedCommandResponse clientResponse;
    batchOp.buildClientResponse(&clientResponse);
    ASSERT(clientResponse.getOk());
    ASSERT_EQUALS(clientResponse.getN(), 1);
    ASSERT(clientResponse.isErrDetailsSet());
    ASSERT_EQUALS(clientResponse.sizeErrDetails(), 1u);
    ASSERT_EQUALS(clientResponse.getErrDetailsAt(0).getStatus().code(),
                  response.getErrDetailsAt(0).getStatus().code());
    ASSERT_EQUALS(clientResponse.getErrDetailsAt(0).getStatus().reason(),
                  response.getErrDetailsAt(0).getStatus().reason());
    ASSERT_EQUALS(clientResponse.getErrDetailsAt(0).getIndex(), 1);
}

// Multi-op targeting test where two ops go to two separate shards and there's an error on each op
// on each shard. There should be one set of two batches to each shard and and two errors reported.
TEST_F(BatchWriteOpTest, MultiOpTwoShardErrorsUnordered) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("foo.bar");
    ShardEndpoint endpointA(ShardId("shardA"),
                            ShardVersionFactory::make(ChunkVersion::IGNORED(), boost::none),
                            boost::none);
    ShardEndpoint endpointB(ShardId("shardB"),
                            ShardVersionFactory::make(ChunkVersion::IGNORED(), boost::none),
                            boost::none);

    auto targeter = initTargeterSplitRange(nss, endpointA, endpointB);

    BatchedCommandRequest request([&] {
        write_ops::InsertCommandRequest insertOp(nss);
        insertOp.setWriteCommandRequestBase([] {
            write_ops::WriteCommandRequestBase wcb;
            wcb.setOrdered(false);
            return wcb;
        }());
        insertOp.setDocuments({BSON("x" << -1), BSON("x" << 1)});
        return insertOp;
    }());

    BatchWriteOp batchOp(_opCtx, request);

    std::map<ShardId, std::unique_ptr<TargetedWriteBatch>> targeted;
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
        batchOp.noteBatchResponse(*it->second, response, nullptr);
    }
    ASSERT(batchOp.isFinished());

    BatchedCommandResponse clientResponse;
    batchOp.buildClientResponse(&clientResponse);
    ASSERT(clientResponse.getOk());
    ASSERT_EQUALS(clientResponse.getN(), 0);
    ASSERT(clientResponse.isErrDetailsSet());
    ASSERT_EQUALS(clientResponse.sizeErrDetails(), 2u);
    ASSERT_EQUALS(clientResponse.getErrDetailsAt(0).getStatus().code(),
                  response.getErrDetailsAt(0).getStatus().code());
    ASSERT_EQUALS(clientResponse.getErrDetailsAt(0).getStatus().reason(),
                  response.getErrDetailsAt(0).getStatus().reason());
    ASSERT_EQUALS(clientResponse.getErrDetailsAt(0).getIndex(), 0);
    ASSERT_EQUALS(clientResponse.getErrDetailsAt(1).getStatus().code(),
                  response.getErrDetailsAt(0).getStatus().code());
    ASSERT_EQUALS(clientResponse.getErrDetailsAt(1).getStatus().reason(),
                  response.getErrDetailsAt(0).getStatus().reason());
    ASSERT_EQUALS(clientResponse.getErrDetailsAt(1).getIndex(), 1);
}

// Multi-op targeting test where each op goes to both shards and there's an error on one op on one
// shard. There should be one set of two batches to each shard and an error reported.
TEST_F(BatchWriteOpTest, MultiOpPartialSingleShardErrorUnordered) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("foo.bar");
    ShardEndpoint endpointA(ShardId("shardA"),
                            ShardVersionFactory::make(ChunkVersion::IGNORED(), boost::none),
                            boost::none);
    ShardEndpoint endpointB(ShardId("shardB"),
                            ShardVersionFactory::make(ChunkVersion::IGNORED(), boost::none),
                            boost::none);

    auto targeter = initTargeterSplitRange(nss, endpointA, endpointB);

    BatchedCommandRequest request([&] {
        write_ops::DeleteCommandRequest deleteOp(nss);
        deleteOp.setWriteCommandRequestBase([] {
            write_ops::WriteCommandRequestBase wcb;
            wcb.setOrdered(false);
            return wcb;
        }());
        deleteOp.setDeletes({buildDelete(BSON("x" << GTE << -1 << LT << 2), true),
                             buildDelete(BSON("x" << GTE << -2 << LT << 1), true)});
        return deleteOp;
    }());

    BatchWriteOp batchOp(_opCtx, request);

    std::map<ShardId, std::unique_ptr<TargetedWriteBatch>> targeted;
    ASSERT_OK(batchOp.targetBatch(targeter, false, &targeted));
    ASSERT(!batchOp.isFinished());
    ASSERT_EQUALS(targeted.size(), 2u);
    verifyTargetedBatches({{endpointA.shardName, 2u}, {endpointB.shardName, 2u}}, targeted);

    // Respond to batches.
    auto targetedIt = targeted.begin();

    BatchedCommandResponse response;
    buildResponse(2, &response);

    // No errors on first shard
    batchOp.noteBatchResponse(*targetedIt->second, response, nullptr);
    ASSERT(!batchOp.isFinished());

    buildResponse(1, &response);
    addError(ErrorCodes::UnknownError, "mock error", 1, &response);

    // Error on second write on second shard
    ++targetedIt;
    batchOp.noteBatchResponse(*targetedIt->second, response, nullptr);
    ASSERT(batchOp.isFinished());
    ASSERT(++targetedIt == targeted.end());

    BatchedCommandResponse clientResponse;
    batchOp.buildClientResponse(&clientResponse);
    ASSERT(clientResponse.getOk());
    ASSERT_EQUALS(clientResponse.getN(), 3);
    ASSERT(clientResponse.isErrDetailsSet());
    ASSERT_EQUALS(clientResponse.sizeErrDetails(), 1u);
    ASSERT_EQUALS(clientResponse.getErrDetailsAt(0).getStatus().code(),
                  response.getErrDetailsAt(0).getStatus().code());
    ASSERT_EQUALS(clientResponse.getErrDetailsAt(0).getStatus().reason(),
                  response.getErrDetailsAt(0).getStatus().reason());
    ASSERT_EQUALS(clientResponse.getErrDetailsAt(0).getIndex(), 1);
}

// Multi-op targeting test where each op goes to both shards and there's an error on one op on one
// shard. There should be one set of two batches to each shard and an error reported, the second op
// should not get run.
TEST_F(BatchWriteOpTest, MultiOpPartialSingleShardErrorOrdered) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("foo.bar");
    ShardEndpoint endpointA(ShardId("shardA"),
                            ShardVersionFactory::make(ChunkVersion::IGNORED(), boost::none),
                            boost::none);
    ShardEndpoint endpointB(ShardId("shardB"),
                            ShardVersionFactory::make(ChunkVersion::IGNORED(), boost::none),
                            boost::none);

    auto targeter = initTargeterSplitRange(nss, endpointA, endpointB);

    BatchedCommandRequest request([&] {
        write_ops::DeleteCommandRequest deleteOp(nss);
        deleteOp.setDeletes({buildDelete(BSON("x" << GTE << -1 << LT << 2), true),
                             buildDelete(BSON("x" << GTE << -2 << LT << 1), true)});
        return deleteOp;
    }());

    BatchWriteOp batchOp(_opCtx, request);

    std::map<ShardId, std::unique_ptr<TargetedWriteBatch>> targeted;
    ASSERT_OK(batchOp.targetBatch(targeter, false, &targeted));
    ASSERT(!batchOp.isFinished());
    ASSERT_EQUALS(targeted.size(), 2u);
    verifyTargetedBatches({{endpointA.shardName, 1u}, {endpointB.shardName, 1u}}, targeted);

    // Respond to batches.
    auto targetedIt = targeted.begin();

    BatchedCommandResponse response;
    buildResponse(1, &response);

    // No errors on first shard
    batchOp.noteBatchResponse(*targetedIt->second, response, nullptr);
    ASSERT(!batchOp.isFinished());

    buildResponse(0, &response);
    addError(ErrorCodes::UnknownError, "mock error", 0, &response);

    // Error on second write on second shard
    ++targetedIt;
    batchOp.noteBatchResponse(*targetedIt->second, response, nullptr);
    ASSERT(batchOp.isFinished());
    ASSERT(++targetedIt == targeted.end());

    BatchedCommandResponse clientResponse;
    batchOp.buildClientResponse(&clientResponse);
    ASSERT(clientResponse.getOk());
    ASSERT_EQUALS(clientResponse.getN(), 1);
    ASSERT(clientResponse.isErrDetailsSet());
    ASSERT_EQUALS(clientResponse.sizeErrDetails(), 1u);
    ASSERT_EQUALS(clientResponse.getErrDetailsAt(0).getStatus().code(),
                  response.getErrDetailsAt(0).getStatus().code());
    ASSERT_EQUALS(clientResponse.getErrDetailsAt(0).getStatus().reason(),
                  response.getErrDetailsAt(0).getStatus().reason());
    ASSERT_EQUALS(clientResponse.getErrDetailsAt(0).getIndex(), 0);
}

//
// Tests of edge-case functionality, lifecycle is assumed to be behaving normally
//

// Multi-op (unordered) error and write concern error test.
TEST_F(BatchWriteOpTest, MultiOpErrorAndWriteConcernErrorUnordered) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("foo.bar");
    ShardEndpoint endpoint(ShardId("shard"),
                           ShardVersionFactory::make(ChunkVersion::IGNORED(), boost::none),
                           boost::none);

    auto targeter = initTargeterFullRange(nss, endpoint);

    BatchedCommandRequest request([&] {
        write_ops::InsertCommandRequest insertOp(nss);
        insertOp.setWriteCommandRequestBase([] {
            write_ops::WriteCommandRequestBase wcb;
            wcb.setOrdered(false);
            return wcb;
        }());
        insertOp.setDocuments({BSON("x" << 1), BSON("x" << 1)});
        return insertOp;
    }());
    _opCtx->setWriteConcern(WriteConcernOptions::parse(BSON("w" << 3)).getValue());

    BatchWriteOp batchOp(_opCtx, request);

    std::map<ShardId, std::unique_ptr<TargetedWriteBatch>> targeted;
    ASSERT_OK(batchOp.targetBatch(targeter, false, &targeted));

    BatchedCommandResponse response;
    buildResponse(1, &response);
    addError(ErrorCodes::UnknownError, "mock error", 1, &response);
    addWCError(&response);

    // First stale response comes back, we should retry
    batchOp.noteBatchResponse(*targeted.begin()->second, response, nullptr);
    ASSERT(batchOp.isFinished());

    // Unordered reports write concern error
    BatchedCommandResponse clientResponse;
    batchOp.buildClientResponse(&clientResponse);
    ASSERT(clientResponse.getOk());
    ASSERT_EQUALS(clientResponse.getN(), 1);
    ASSERT_EQUALS(clientResponse.sizeErrDetails(), 1u);
    ASSERT(clientResponse.isWriteConcernErrorSet());
}

// Single-op (ordered) error and write concern error test.
TEST_F(BatchWriteOpTest, SingleOpErrorAndWriteConcernErrorOrdered) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("foo.bar");
    ShardEndpoint endpointA(ShardId("shardA"),
                            ShardVersionFactory::make(ChunkVersion::IGNORED(), boost::none),
                            boost::none);
    ShardEndpoint endpointB(ShardId("shardB"),
                            ShardVersionFactory::make(ChunkVersion::IGNORED(), boost::none),
                            boost::none);

    auto targeter = initTargeterSplitRange(nss, endpointA, endpointB);

    BatchedCommandRequest request([&] {
        write_ops::UpdateCommandRequest updateOp(nss);
        updateOp.setWriteCommandRequestBase([] {
            write_ops::WriteCommandRequestBase wcb;
            wcb.setOrdered(false);
            return wcb;
        }());
        updateOp.setUpdates({buildUpdate(BSON("x" << GTE << -1 << LT << 2), true)});
        return updateOp;
    }());
    _opCtx->setWriteConcern(WriteConcernOptions::parse(BSON("w" << 3)).getValue());

    BatchWriteOp batchOp(_opCtx, request);

    std::map<ShardId, std::unique_ptr<TargetedWriteBatch>> targeted;
    ASSERT_OK(batchOp.targetBatch(targeter, false, &targeted));

    // Respond to batches.
    auto targetedIt = targeted.begin();

    BatchedCommandResponse response;
    buildResponse(1, &response);
    addWCError(&response);

    // First response comes back with write concern error
    batchOp.noteBatchResponse(*targetedIt->second, response, nullptr);
    ASSERT(!batchOp.isFinished());

    buildResponse(0, &response);
    addError(ErrorCodes::UnknownError, "mock error", 0, &response);

    // Second response comes back with write error
    ++targetedIt;
    batchOp.noteBatchResponse(*targetedIt->second, response, nullptr);
    ASSERT(batchOp.isFinished());
    ASSERT(++targetedIt == targeted.end());

    // Ordered reports write concern error.
    BatchedCommandResponse clientResponse;
    batchOp.buildClientResponse(&clientResponse);
    ASSERT(clientResponse.getOk());
    ASSERT_EQUALS(clientResponse.getN(), 1);
    ASSERT(clientResponse.isErrDetailsSet());
    ASSERT_EQUALS(clientResponse.sizeErrDetails(), 1u);
    ASSERT(clientResponse.isWriteConcernErrorSet());
}

// Targeting failure on second op in batch op (ordered)
TEST_F(BatchWriteOpTest, MultiOpFailedTargetOrdered) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("foo.bar");
    ShardEndpoint endpoint(ShardId("shard"),
                           ShardVersionFactory::make(ChunkVersion::IGNORED(), boost::none),
                           boost::none);

    auto targeter = initTargeterHalfRange(nss, endpoint);

    BatchedCommandRequest request([&] {
        write_ops::InsertCommandRequest insertOp(nss);
        insertOp.setDocuments({BSON("x" << -1), BSON("x" << 2), BSON("x" << -2)});
        return insertOp;
    }());

    // Do single-target, multi-doc batch write op

    BatchWriteOp batchOp(_opCtx, request);

    std::map<ShardId, std::unique_ptr<TargetedWriteBatch>> targeted;
    ASSERT_NOT_OK(batchOp.targetBatch(targeter, false, &targeted));

    // First targeting round fails since we may be stale
    ASSERT(!batchOp.isFinished());

    targeted.clear();
    ASSERT_OK(batchOp.targetBatch(targeter, true, &targeted));

    // Second targeting round is ok, but should stop at first write
    ASSERT(!batchOp.isFinished());
    ASSERT_EQUALS(targeted.size(), 1u);
    ASSERT_EQUALS(targeted.begin()->second->getWrites().size(), 1u);

    BatchedCommandResponse response;
    buildResponse(1, &response);

    // First response ok
    batchOp.noteBatchResponse(*targeted.begin()->second, response, nullptr);
    ASSERT(!batchOp.isFinished());

    targeted.clear();
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
    ASSERT_EQUALS(clientResponse.getErrDetailsAt(0).getIndex(), 1);
}

// Targeting failure on second op in batch op (unordered)
TEST_F(BatchWriteOpTest, MultiOpFailedTargetUnordered) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("foo.bar");
    ShardEndpoint endpoint(ShardId("shard"),
                           ShardVersionFactory::make(ChunkVersion::IGNORED(), boost::none),
                           boost::none);

    auto targeter = initTargeterHalfRange(nss, endpoint);

    BatchedCommandRequest request([&] {
        write_ops::InsertCommandRequest insertOp(nss);
        insertOp.setWriteCommandRequestBase([] {
            write_ops::WriteCommandRequestBase wcb;
            wcb.setOrdered(false);
            return wcb;
        }());
        insertOp.setDocuments({BSON("x" << -1), BSON("x" << 2), BSON("x" << -2)});
        return insertOp;
    }());

    // Do single-target, multi-doc batch write op

    BatchWriteOp batchOp(_opCtx, request);

    std::map<ShardId, std::unique_ptr<TargetedWriteBatch>> targeted;
    ASSERT_NOT_OK(batchOp.targetBatch(targeter, false, &targeted));

    // First targeting round fails since we may be stale
    ASSERT(!batchOp.isFinished());

    targeted.clear();
    ASSERT_OK(batchOp.targetBatch(targeter, true, &targeted));

    // Second targeting round is ok, and should record an error
    ASSERT(!batchOp.isFinished());
    ASSERT_EQUALS(targeted.size(), 1u);
    ASSERT_EQUALS(targeted.begin()->second->getWrites().size(), 2u);

    BatchedCommandResponse response;
    buildResponse(2, &response);

    // Response is ok for first and third write
    batchOp.noteBatchResponse(*targeted.begin()->second, response, nullptr);
    ASSERT(batchOp.isFinished());

    BatchedCommandResponse clientResponse;
    batchOp.buildClientResponse(&clientResponse);
    ASSERT(clientResponse.getOk());
    ASSERT_EQUALS(clientResponse.getN(), 2);
    ASSERT(clientResponse.isErrDetailsSet());
    ASSERT_EQUALS(clientResponse.sizeErrDetails(), 1u);
    ASSERT_EQUALS(clientResponse.getErrDetailsAt(0).getIndex(), 1);
}

// Batch failure (ok : 0) reported in a multi-op batch (ordered). Expect this gets translated down
// into write errors for first affected write.
TEST_F(BatchWriteOpTest, MultiOpFailedBatchOrdered) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("foo.bar");
    ShardEndpoint endpointA(ShardId("shardA"),
                            ShardVersionFactory::make(ChunkVersion::IGNORED(), boost::none),
                            boost::none);
    ShardEndpoint endpointB(ShardId("shardB"),
                            ShardVersionFactory::make(ChunkVersion::IGNORED(), boost::none),
                            boost::none);

    auto targeter = initTargeterSplitRange(nss, endpointA, endpointB);

    BatchedCommandRequest request([&] {
        write_ops::InsertCommandRequest insertOp(nss);
        insertOp.setDocuments({BSON("x" << -1), BSON("x" << 2), BSON("x" << 3)});
        return insertOp;
    }());

    BatchWriteOp batchOp(_opCtx, request);

    std::map<ShardId, std::unique_ptr<TargetedWriteBatch>> targeted;
    ASSERT_OK(batchOp.targetBatch(targeter, false, &targeted));

    BatchedCommandResponse response;
    buildResponse(1, &response);

    // First shard batch is ok
    batchOp.noteBatchResponse(*targeted.begin()->second, response, nullptr);
    ASSERT(!batchOp.isFinished());

    targeted.clear();
    ASSERT_OK(batchOp.targetBatch(targeter, true, &targeted));

    buildErrResponse(ErrorCodes::UnknownError, "mock error", &response);

    // Second shard batch fails
    batchOp.noteBatchResponse(*targeted.begin()->second, response, nullptr);
    ASSERT(batchOp.isFinished());

    // We should have recorded an error for the second write
    BatchedCommandResponse clientResponse;
    batchOp.buildClientResponse(&clientResponse);
    ASSERT(clientResponse.getOk());
    ASSERT_EQUALS(clientResponse.getN(), 1);
    ASSERT(clientResponse.isErrDetailsSet());
    ASSERT_EQUALS(clientResponse.sizeErrDetails(), 1u);
    ASSERT_EQUALS(clientResponse.getErrDetailsAt(0).getIndex(), 1);
    ASSERT_EQUALS(clientResponse.getErrDetailsAt(0).getStatus().code(), response.toStatus().code());
}

// Batch failure (ok : 0) reported in a multi-op batch (unordered). Expect this gets translated down
// into write errors for all affected writes.
TEST_F(BatchWriteOpTest, MultiOpFailedBatchUnordered) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("foo.bar");
    ShardEndpoint endpointA(ShardId("shardA"),
                            ShardVersionFactory::make(ChunkVersion::IGNORED(), boost::none),
                            boost::none);
    ShardEndpoint endpointB(ShardId("shardB"),
                            ShardVersionFactory::make(ChunkVersion::IGNORED(), boost::none),
                            boost::none);

    auto targeter = initTargeterSplitRange(nss, endpointA, endpointB);

    BatchedCommandRequest request([&] {
        write_ops::InsertCommandRequest insertOp(nss);
        insertOp.setWriteCommandRequestBase([] {
            write_ops::WriteCommandRequestBase wcb;
            wcb.setOrdered(false);
            return wcb;
        }());
        insertOp.setDocuments({BSON("x" << -1), BSON("x" << 2), BSON("x" << 3)});
        return insertOp;
    }());

    BatchWriteOp batchOp(_opCtx, request);

    std::map<ShardId, std::unique_ptr<TargetedWriteBatch>> targeted;
    ASSERT_OK(batchOp.targetBatch(targeter, false, &targeted));

    // Respond to batches.
    auto targetedIt = targeted.begin();

    BatchedCommandResponse response;
    buildResponse(1, &response);

    // First shard batch is ok
    batchOp.noteBatchResponse(*targetedIt->second, response, nullptr);
    ASSERT(!batchOp.isFinished());

    buildErrResponse(ErrorCodes::UnknownError, "mock error", &response);

    // Second shard batch fails
    ++targetedIt;
    batchOp.noteBatchResponse(*targetedIt->second, response, nullptr);
    ASSERT(batchOp.isFinished());
    ASSERT(++targetedIt == targeted.end());

    // We should have recorded an error for the second and third write
    BatchedCommandResponse clientResponse;
    batchOp.buildClientResponse(&clientResponse);
    ASSERT(clientResponse.getOk());
    ASSERT_EQUALS(clientResponse.getN(), 1);
    ASSERT(clientResponse.isErrDetailsSet());
    ASSERT_EQUALS(clientResponse.sizeErrDetails(), 2u);
    ASSERT_EQUALS(clientResponse.getErrDetailsAt(0).getIndex(), 1);
    ASSERT_EQUALS(clientResponse.getErrDetailsAt(0).getStatus().code(), response.toStatus().code());
    ASSERT_EQUALS(clientResponse.getErrDetailsAt(1).getIndex(), 2);
    ASSERT_EQUALS(clientResponse.getErrDetailsAt(1).getStatus().code(), response.toStatus().code());
}

// Batch aborted (ordered). Expect this gets translated down into write error for first affected
// write.
TEST_F(BatchWriteOpTest, MultiOpAbortOrdered) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("foo.bar");
    ShardEndpoint endpointA(ShardId("shardA"),
                            ShardVersionFactory::make(ChunkVersion::IGNORED(), boost::none),
                            boost::none);
    ShardEndpoint endpointB(ShardId("shardB"),
                            ShardVersionFactory::make(ChunkVersion::IGNORED(), boost::none),
                            boost::none);

    auto targeter = initTargeterSplitRange(nss, endpointA, endpointB);

    BatchedCommandRequest request([&] {
        write_ops::InsertCommandRequest insertOp(nss);
        insertOp.setDocuments({BSON("x" << -1), BSON("x" << 2), BSON("x" << 3)});
        return insertOp;
    }());

    BatchWriteOp batchOp(_opCtx, request);

    std::map<ShardId, std::unique_ptr<TargetedWriteBatch>> targeted;
    ASSERT_OK(batchOp.targetBatch(targeter, false, &targeted));

    BatchedCommandResponse response;
    buildResponse(1, &response);

    // First shard batch is ok
    batchOp.noteBatchResponse(*targeted.begin()->second, response, nullptr);
    ASSERT(!batchOp.isFinished());

    write_ops::WriteError abortError(0, {ErrorCodes::UnknownError, "mock abort"});
    batchOp.abortBatch(abortError);
    ASSERT(batchOp.isFinished());

    // We should have recorded an error for the second write
    BatchedCommandResponse clientResponse;
    batchOp.buildClientResponse(&clientResponse);
    ASSERT(clientResponse.getOk());
    ASSERT_EQUALS(clientResponse.getN(), 1);
    ASSERT(clientResponse.isErrDetailsSet());
    ASSERT_EQUALS(clientResponse.sizeErrDetails(), 1u);
    ASSERT_EQUALS(clientResponse.getErrDetailsAt(0).getIndex(), 1);
    ASSERT_EQUALS(clientResponse.getErrDetailsAt(0).getStatus().code(),
                  abortError.getStatus().code());
}

// Batch aborted (unordered). Expect this gets translated down into write errors for all affected
// writes.
TEST_F(BatchWriteOpTest, MultiOpAbortUnordered) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("foo.bar");
    ShardEndpoint endpointA(ShardId("shardA"),
                            ShardVersionFactory::make(ChunkVersion::IGNORED(), boost::none),
                            boost::none);
    ShardEndpoint endpointB(ShardId("shardB"),
                            ShardVersionFactory::make(ChunkVersion::IGNORED(), boost::none),
                            boost::none);

    auto targeter = initTargeterSplitRange(nss, endpointA, endpointB);

    BatchedCommandRequest request([&] {
        write_ops::InsertCommandRequest insertOp(nss);
        insertOp.setWriteCommandRequestBase([] {
            write_ops::WriteCommandRequestBase wcb;
            wcb.setOrdered(false);
            return wcb;
        }());
        insertOp.setDocuments({BSON("x" << -1), BSON("x" << -2)});
        return insertOp;
    }());

    BatchWriteOp batchOp(_opCtx, request);

    write_ops::WriteError abortError(0, {ErrorCodes::UnknownError, "mock abort"});
    batchOp.abortBatch(abortError);
    ASSERT(batchOp.isFinished());

    // We should have recorded an error for the first and second write
    BatchedCommandResponse clientResponse;
    batchOp.buildClientResponse(&clientResponse);
    ASSERT(clientResponse.getOk());
    ASSERT_EQUALS(clientResponse.getN(), 0);
    ASSERT(clientResponse.isErrDetailsSet());
    ASSERT_EQUALS(clientResponse.sizeErrDetails(), 2u);
    ASSERT_EQUALS(clientResponse.getErrDetailsAt(0).getIndex(), 0);
    ASSERT_EQUALS(clientResponse.getErrDetailsAt(0).getStatus().code(),
                  abortError.getStatus().code());
    ASSERT_EQUALS(clientResponse.getErrDetailsAt(1).getIndex(), 1);
    ASSERT_EQUALS(clientResponse.getErrDetailsAt(1).getStatus().code(),
                  abortError.getStatus().code());
}

// Multi-op targeting test where each op goes to both shards and both return a write concern error
TEST_F(BatchWriteOpTest, MultiOpTwoWCErrors) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("foo.bar");
    ShardEndpoint endpointA(ShardId("shardA"),
                            ShardVersionFactory::make(ChunkVersion::IGNORED(), boost::none),
                            boost::none);
    ShardEndpoint endpointB(ShardId("shardB"),
                            ShardVersionFactory::make(ChunkVersion::IGNORED(), boost::none),
                            boost::none);

    auto targeter = initTargeterSplitRange(nss, endpointA, endpointB);

    BatchedCommandRequest request([&] {
        write_ops::InsertCommandRequest insertOp(nss);
        insertOp.setDocuments({BSON("x" << -1), BSON("x" << 2)});
        return insertOp;
    }());
    _opCtx->setWriteConcern(WriteConcernOptions::parse(BSON("w" << 3)).getValue());

    BatchWriteOp batchOp(_opCtx, request);

    std::map<ShardId, std::unique_ptr<TargetedWriteBatch>> targeted;
    ASSERT_OK(batchOp.targetBatch(targeter, false, &targeted));

    BatchedCommandResponse response;
    buildResponse(1, &response);
    addWCError(&response);

    // First shard write write concern fails.
    batchOp.noteBatchResponse(*targeted.begin()->second, response, nullptr);
    ASSERT(!batchOp.isFinished());

    targeted.clear();
    ASSERT_OK(batchOp.targetBatch(targeter, true, &targeted));

    // Second shard write write concern fails.
    batchOp.noteBatchResponse(*targeted.begin()->second, response, nullptr);
    ASSERT(batchOp.isFinished());

    BatchedCommandResponse clientResponse;
    batchOp.buildClientResponse(&clientResponse);
    ASSERT(clientResponse.getOk());
    ASSERT_EQUALS(clientResponse.getN(), 2);
    ASSERT(!clientResponse.isErrDetailsSet());
    ASSERT(clientResponse.isWriteConcernErrorSet());
}

TEST_F(BatchWriteOpTest, AttachingStmtIds) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("foo.bar");
    ShardEndpoint endpoint(ShardId("shard"),
                           ShardVersionFactory::make(ChunkVersion::IGNORED(), boost::none),
                           boost::none);
    auto targeter = initTargeterFullRange(nss, endpoint);

    const std::vector<StmtId> stmtIds{1, 2, 3};
    const std::vector<LogicalSessionId> lsids{
        makeLogicalSessionIdForTest(),
        makeLogicalSessionIdWithTxnUUIDForTest(),
        makeLogicalSessionIdWithTxnNumberAndUUIDForTest(),
    };
    const BatchedCommandRequest originalRequest([&] {
        write_ops::InsertCommandRequest insertOp(nss);
        insertOp.setDocuments({BSON("x" << 1), BSON("x" << 2), BSON("x" << 3)});
        insertOp.getWriteCommandRequestBase().setStmtIds({stmtIds});
        return insertOp;
    }());

    auto makeTargetedBatchedCommandRequest = [&] {
        BatchWriteOp batchOp(_opCtx, originalRequest);

        std::map<ShardId, std::unique_ptr<TargetedWriteBatch>> targeted;
        ASSERT_OK(batchOp.targetBatch(targeter, false, &targeted));
        ASSERT(!batchOp.isFinished());
        ASSERT_EQUALS(targeted.size(), 1u);
        assertEndpointsEqual(getFirstTargetedWriteEndpoint(targeted.begin()->second), endpoint);

        BatchedCommandRequest targetedRequest =
            batchOp.buildBatchRequest(*targeted.begin()->second, targeter, boost::none);
        return targetedRequest;
    };

    {
        // Verify that when the command is not running in a session, the targeted batched command
        // request does not have stmtIds attached to it.
        auto request = makeTargetedBatchedCommandRequest();
        ASSERT_FALSE(request.getInsertRequest().getStmtIds());
    }

    {
        // Verify that when the command is running in a session but not as retryable writes or in a
        // retryable internal transaction, the targeted batched command request does not have
        // stmtIds to it.
        for (auto& lsid : lsids) {
            _opCtx->setLogicalSessionId(lsid);
            auto targetedRequest = makeTargetedBatchedCommandRequest();
            ASSERT_FALSE(targetedRequest.getInsertRequest().getStmtIds());
        }
    }

    {
        // Verify that when the command is running in a session as retryable writes, the targeted
        // batched command request has stmtIds attached to it.
        _opCtx->setTxnNumber(0);
        for (auto& lsid : lsids) {
            _opCtx->setLogicalSessionId(lsid);
            auto targetedRequest = makeTargetedBatchedCommandRequest();
            auto requestStmtIds = targetedRequest.getInsertRequest().getStmtIds();
            ASSERT(requestStmtIds);
            ASSERT(*requestStmtIds == stmtIds);
        }
    }

    {
        // Verify that when the command is running in a transaction, the targeted batched command
        // request has stmtIds attached to it if and only if the transaction is a retryable internal
        // transaction.
        _opCtx->setTxnNumber(0);
        _opCtx->setInMultiDocumentTransaction();
        for (auto& lsid : lsids) {
            _opCtx->setLogicalSessionId(lsid);
            auto request = makeTargetedBatchedCommandRequest();
            auto requestStmtIds = request.getInsertRequest().getStmtIds();
            if (isInternalSessionForRetryableWrite(lsid)) {
                ASSERT(requestStmtIds);
                ASSERT(*requestStmtIds == stmtIds);
            } else {
                ASSERT_FALSE(requestStmtIds);
            }
        }
    }
}

//
// Tests of batch size limit functionality
//

using BatchWriteOpLimitTests = WriteOpTestFixture;

// Big single operation test - should go through
TEST_F(BatchWriteOpLimitTests, OneBigDoc) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("foo.bar");
    ShardEndpoint endpoint(ShardId("shard"),
                           ShardVersionFactory::make(ChunkVersion::IGNORED(), boost::none),
                           boost::none);

    auto targeter = initTargeterFullRange(nss, endpoint);

    // Create a BSONObj (slightly) bigger than the maximum size by including a max-size string
    const std::string bigString(BSONObjMaxUserSize, 'x');

    // Do single-target, single doc batch write op
    BatchedCommandRequest request([&] {
        write_ops::InsertCommandRequest insertOp(nss);
        insertOp.setWriteCommandRequestBase([] {
            write_ops::WriteCommandRequestBase wcb;
            wcb.setOrdered(false);
            return wcb;
        }());
        insertOp.setDocuments({BSON("x" << 1 << "data" << bigString)});
        return insertOp;
    }());

    BatchWriteOp batchOp(_opCtx, request);

    std::map<ShardId, std::unique_ptr<TargetedWriteBatch>> targeted;
    ASSERT_OK(batchOp.targetBatch(targeter, false, &targeted));
    ASSERT_EQUALS(targeted.size(), 1u);

    BatchedCommandResponse response;
    buildResponse(1, &response);

    batchOp.noteBatchResponse(*targeted.begin()->second, response, nullptr);
    ASSERT(batchOp.isFinished());
}

// Big doc with smaller additional doc - should go through as two batches
TEST_F(BatchWriteOpLimitTests, OneBigOneSmall) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("foo.bar");
    ShardEndpoint endpoint(ShardId("shard"),
                           ShardVersionFactory::make(ChunkVersion::IGNORED(), boost::none),
                           boost::none);

    auto targeter = initTargeterFullRange(nss, endpoint);

    // Create a BSONObj (slightly) bigger than the maximum size by including a max-size string
    const std::string bigString(BSONObjMaxUserSize, 'x');

    BatchedCommandRequest request([&] {
        write_ops::UpdateCommandRequest updateOp(nss);
        updateOp.setUpdates({buildUpdate(BSON("x" << 1), BSON("data" << bigString), false),
                             buildUpdate(BSON("x" << 2), BSONObj(), false)});
        return updateOp;
    }());

    BatchWriteOp batchOp(_opCtx, request);

    std::map<ShardId, std::unique_ptr<TargetedWriteBatch>> targeted;
    ASSERT_OK(batchOp.targetBatch(targeter, false, &targeted));
    ASSERT_EQUALS(targeted.size(), 1u);
    ASSERT_EQUALS(targeted.begin()->second->getWrites().size(), 1u);

    BatchedCommandResponse response;
    buildResponse(1, &response);

    batchOp.noteBatchResponse(*targeted.begin()->second, response, nullptr);
    ASSERT(!batchOp.isFinished());

    targeted.clear();
    ASSERT_OK(batchOp.targetBatch(targeter, false, &targeted));
    ASSERT_EQUALS(targeted.size(), 1u);
    ASSERT_EQUALS(targeted.begin()->second->getWrites().size(), 1u);

    batchOp.noteBatchResponse(*targeted.begin()->second, response, nullptr);
    ASSERT(batchOp.isFinished());
}

class BatchWriteOpTransactionTest : public ShardingTestFixture {
public:
    const TxnNumber kTxnNumber = 5;

    void setUp() override {
        ShardingTestFixture::setUp();

        operationContext()->setLogicalSessionId(makeLogicalSessionIdForTest());
        operationContext()->setTxnNumber(kTxnNumber);
        repl::ReadConcernArgs::get(operationContext()) =
            repl::ReadConcernArgs(repl::ReadConcernLevel::kSnapshotReadConcern);

        _scopedSession.emplace(operationContext());

        auto txnRouter = TransactionRouter::get(operationContext());
        txnRouter.beginOrContinueTxn(
            operationContext(), kTxnNumber, TransactionRouter::TransactionActions::kStart);
    }

    void tearDown() override {
        _scopedSession.reset();
        repl::ReadConcernArgs::get(operationContext()) = repl::ReadConcernArgs();

        ShardingTestFixture::tearDown();
    }

private:
    boost::optional<RouterOperationContextSession> _scopedSession;
};

TEST_F(BatchWriteOpTransactionTest, ThrowTargetingErrorsInTransaction_Delete) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("foo.bar");
    ShardEndpoint endpoint(ShardId("shard"),
                           ShardVersionFactory::make(ChunkVersion::IGNORED(), boost::none),
                           boost::none);

    auto targeter = initTargeterHalfRange(nss, endpoint);

    // Untargetable delete op.
    BatchedCommandRequest deleteRequest([&] {
        write_ops::DeleteCommandRequest deleteOp(nss);
        deleteOp.setDeletes({buildDelete(BSON("x" << 1), false)});
        return deleteOp;
    }());
    BatchWriteOp batchOp(operationContext(), deleteRequest);

    std::map<ShardId, std::unique_ptr<TargetedWriteBatch>> targeted;

    auto status = batchOp.targetBatch(targeter, false, &targeted);
    ASSERT_EQ(ErrorCodes::UnknownError, status.getStatus().code());

    BatchedCommandResponse response;
    batchOp.buildClientResponse(&response);

    ASSERT(response.isErrDetailsSet());
    ASSERT_GT(response.sizeErrDetails(), 0u);
    ASSERT_EQ(ErrorCodes::UnknownError, response.getErrDetailsAt(0).getStatus().code());
}

TEST_F(BatchWriteOpTransactionTest, ThrowTargetingErrorsInTransaction_Update) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("foo.bar");
    ShardEndpoint endpoint(ShardId("shard"),
                           ShardVersionFactory::make(ChunkVersion::IGNORED(), boost::none),
                           boost::none);

    auto targeter = initTargeterHalfRange(nss, endpoint);

    // Untargetable update op.
    BatchedCommandRequest updateRequest([&] {
        write_ops::UpdateCommandRequest updateOp(nss);
        updateOp.setUpdates({buildUpdate(BSON("x" << 1), BSONObj(), false)});
        return updateOp;
    }());
    BatchWriteOp batchOp(operationContext(), updateRequest);

    std::map<ShardId, std::unique_ptr<TargetedWriteBatch>> targeted;

    auto status = batchOp.targetBatch(targeter, false, &targeted);
    ASSERT_EQ(ErrorCodes::UnknownError, status.getStatus().code());

    BatchedCommandResponse response;
    batchOp.buildClientResponse(&response);

    ASSERT(response.isErrDetailsSet());
    ASSERT_GT(response.sizeErrDetails(), 0u);
    ASSERT_EQ(ErrorCodes::UnknownError, response.getErrDetailsAt(0).getStatus().code());
}

class WriteWithoutShardKeyFixture : public RouterCatalogCacheTestFixture {
public:
    void setUp() override {
        RouterCatalogCacheTestFixture::setUp();
        const ShardKeyPattern shardKeyPattern(BSON("x" << 1));
        makeCollectionRoutingInfo(
            kNss, shardKeyPattern, nullptr, false, {BSON("x" << splitPoint)}, {});
        _criTargeter = CollectionRoutingInfoTargeter(operationContext(), kNss);
    }

    OperationContext* getOpCtx() {
        return operationContext();
    }
    CollectionRoutingInfoTargeter getCollectionRoutingInfoTargeter() const {
        return *_criTargeter;
    }


private:
    boost::optional<CollectionRoutingInfoTargeter> _criTargeter;
};

TEST_F(WriteWithoutShardKeyFixture, SingleUpdateWithoutShardKey) {
    // Do single-target, single doc batch write op
    BatchedCommandRequest request([&] {
        write_ops::UpdateCommandRequest updateOp(kNss);
        updateOp.setUpdates({buildUpdate(BSON("y" << 1), BSONObj(), false)});
        return updateOp;
    }());

    BatchWriteOp batchOp(getOpCtx(), request);

    std::map<ShardId, std::unique_ptr<TargetedWriteBatch>> targeted;
    auto status = batchOp.targetBatch(getCollectionRoutingInfoTargeter(), false, &targeted);
    ASSERT_OK(status.getStatus());
    ASSERT_EQUALS(status.getValue(), WriteType::WithoutShardKeyOrId);
    ASSERT(!batchOp.isFinished());
    ASSERT_EQUALS(targeted.size(), 2u);
}

TEST_F(WriteWithoutShardKeyFixture, MultipleOrderedUpdateWithoutShardKey) {
    // Do single-target, multi doc batch write op
    BatchedCommandRequest request([&] {
        write_ops::UpdateCommandRequest updateOp(kNss);
        updateOp.setWriteCommandRequestBase([] {
            write_ops::WriteCommandRequestBase wcb;
            wcb.setOrdered(true);
            return wcb;
        }());
        updateOp.setUpdates({buildUpdate(BSON("y" << 1), BSONObj(), false),
                             buildUpdate(BSON("y" << 1), BSONObj(), false)});
        return updateOp;
    }());

    BatchWriteOp batchOp(getOpCtx(), request);

    std::map<ShardId, std::unique_ptr<TargetedWriteBatch>> targeted;
    auto status = batchOp.targetBatch(getCollectionRoutingInfoTargeter(), false, &targeted);
    ASSERT_OK(status);
    ASSERT_EQUALS(status.getValue(), WriteType::WithoutShardKeyOrId);
    ASSERT(!batchOp.isFinished());
    ASSERT_EQUALS(targeted.size(), 2u);

    BatchedCommandResponse response;
    buildResponse(1, &response);

    // Respond to first targeted batch
    for (const auto& e : targeted) {
        batchOp.noteBatchResponse(*(e.second), response, nullptr);
    }

    ASSERT(!batchOp.isFinished());

    targeted.clear();

    auto status2 = batchOp.targetBatch(getCollectionRoutingInfoTargeter(), false, &targeted);
    ASSERT_OK(status2);
    ASSERT_EQUALS(status2.getValue(), WriteType::WithoutShardKeyOrId);
    ASSERT(!batchOp.isFinished());
    ASSERT_EQUALS(targeted.size(), 2u);

    // Respond to second targeted batch
    for (const auto& e : targeted) {
        batchOp.noteBatchResponse(*(e.second), response, nullptr);
    }
    ASSERT(batchOp.isFinished());
}

TEST_F(WriteWithoutShardKeyFixture, MultipleUnorderedUpdateWithoutShardKey) {
    // Do single-target, multi doc batch write op
    BatchedCommandRequest request([&] {
        write_ops::UpdateCommandRequest updateOp(kNss);
        updateOp.setWriteCommandRequestBase([] {
            write_ops::WriteCommandRequestBase wcb;
            wcb.setOrdered(false);
            return wcb;
        }());
        updateOp.setUpdates({buildUpdate(BSON("y" << 1), BSONObj(), false),
                             buildUpdate(BSON("y" << 1), BSONObj(), false)});
        return updateOp;
    }());

    BatchWriteOp batchOp(getOpCtx(), request);

    std::map<ShardId, std::unique_ptr<TargetedWriteBatch>> targeted;
    auto status = batchOp.targetBatch(getCollectionRoutingInfoTargeter(), false, &targeted);
    ASSERT_OK(status);
    ASSERT_EQUALS(status.getValue(), WriteType::WithoutShardKeyOrId);
    ASSERT(!batchOp.isFinished());
    ASSERT_EQUALS(targeted.size(), 2u);

    BatchedCommandResponse response;
    buildResponse(1, &response);

    // Respond to first targeted batch
    for (const auto& e : targeted) {
        batchOp.noteBatchResponse(*(e.second), response, nullptr);
    }
    ASSERT(!batchOp.isFinished());

    targeted.clear();

    auto status2 = batchOp.targetBatch(getCollectionRoutingInfoTargeter(), false, &targeted);
    ASSERT_OK(status2);
    ASSERT_EQUALS(status2.getValue(), WriteType::WithoutShardKeyOrId);
    ASSERT(!batchOp.isFinished());
    ASSERT_EQUALS(targeted.size(), 2u);
    ASSERT_EQUALS(targeted.begin()->second->getWrites().size(), 1u);

    // Respond to second targeted batch
    for (const auto& e : targeted) {
        batchOp.noteBatchResponse(*(e.second), response, nullptr);
    }
    ASSERT(batchOp.isFinished());
}

TEST_F(WriteWithoutShardKeyFixture, SingleDeleteWithoutShardKey) {
    // Do single-target, single doc batch write op
    BatchedCommandRequest request([&] {
        write_ops::DeleteCommandRequest deleteOp(kNss);
        deleteOp.setDeletes({buildDelete(BSON("y" << 1), false)});
        return deleteOp;
    }());

    BatchWriteOp batchOp(getOpCtx(), request);

    std::map<ShardId, std::unique_ptr<TargetedWriteBatch>> targeted;
    auto status = batchOp.targetBatch(getCollectionRoutingInfoTargeter(), false, &targeted);
    ASSERT_OK(status);
    ASSERT_EQUALS(status.getValue(), WriteType::WithoutShardKeyOrId);
    ASSERT(!batchOp.isFinished());
    ASSERT_EQUALS(targeted.size(), 2u);
}


TEST_F(WriteWithoutShardKeyFixture, MultipleOrderedDeletesWithoutShardKey) {
    // Do single-target, multi doc batch write op
    BatchedCommandRequest request([&] {
        write_ops::DeleteCommandRequest deleteOp(kNss);
        deleteOp.setWriteCommandRequestBase([] {
            write_ops::WriteCommandRequestBase wcb;
            wcb.setOrdered(true);
            return wcb;
        }());
        deleteOp.setDeletes(
            {buildDelete(BSON("y" << 1), false), buildDelete(BSON("y" << 1), false)});
        return deleteOp;
    }());

    BatchWriteOp batchOp(getOpCtx(), request);

    std::map<ShardId, std::unique_ptr<TargetedWriteBatch>> targeted;
    auto status = batchOp.targetBatch(getCollectionRoutingInfoTargeter(), false, &targeted);
    ASSERT_OK(status);
    ASSERT_EQUALS(status.getValue(), WriteType::WithoutShardKeyOrId);
    ASSERT(!batchOp.isFinished());
    ASSERT_EQUALS(targeted.size(), 2u);

    BatchedCommandResponse response;
    buildResponse(1, &response);

    // Respond to first targeted batch
    for (const auto& e : targeted) {
        batchOp.noteBatchResponse(*(e.second), response, nullptr);
    }
    ASSERT(!batchOp.isFinished());

    targeted.clear();

    auto status2 = batchOp.targetBatch(getCollectionRoutingInfoTargeter(), false, &targeted);
    ASSERT_OK(status2);
    ASSERT_EQUALS(status2.getValue(), WriteType::WithoutShardKeyOrId);
    ASSERT(!batchOp.isFinished());
    ASSERT_EQUALS(targeted.size(), 2u);

    // Respond to second targeted batch
    for (const auto& e : targeted) {
        batchOp.noteBatchResponse(*(e.second), response, nullptr);
    }
    ASSERT(batchOp.isFinished());
}

TEST_F(WriteWithoutShardKeyFixture, MultipleUnorderedDeletesWithoutShardKey) {
    // Do single-target, multi doc batch write op
    BatchedCommandRequest request([&] {
        write_ops::DeleteCommandRequest deleteOp(kNss);
        deleteOp.setWriteCommandRequestBase([] {
            write_ops::WriteCommandRequestBase wcb;
            wcb.setOrdered(false);
            return wcb;
        }());
        deleteOp.setDeletes(
            {buildDelete(BSON("y" << 1), false), buildDelete(BSON("y" << 1), false)});
        return deleteOp;
    }());

    BatchWriteOp batchOp(getOpCtx(), request);

    std::map<ShardId, std::unique_ptr<TargetedWriteBatch>> targeted;
    auto status = batchOp.targetBatch(getCollectionRoutingInfoTargeter(), false, &targeted);
    ASSERT_OK(status);
    ASSERT_EQUALS(status.getValue(), WriteType::WithoutShardKeyOrId);
    ASSERT(!batchOp.isFinished());
    ASSERT_EQUALS(targeted.size(), 2u);

    BatchedCommandResponse response;
    buildResponse(1, &response);

    // Respond to first targeted batch
    batchOp.noteBatchResponse(*targeted.begin()->second, response, nullptr);
    for (const auto& e : targeted) {
        batchOp.noteBatchResponse(*(e.second), response, nullptr);
    }

    targeted.clear();

    auto status2 = batchOp.targetBatch(getCollectionRoutingInfoTargeter(), false, &targeted);
    ASSERT_OK(status2);
    ASSERT_EQUALS(status2.getValue(), WriteType::WithoutShardKeyOrId);
    ASSERT(!batchOp.isFinished());
    ASSERT_EQUALS(targeted.size(), 2u);

    // Respond to second targeted batch
    batchOp.noteBatchResponse(*targeted.begin()->second, response, nullptr);
    for (const auto& e : targeted) {
        batchOp.noteBatchResponse(*(e.second), response, nullptr);
    }
}

//
// Tests targeting for retryable time-series updates.
//
class TimeseriesRetryableUpdateFixture : public RouterCatalogCacheTestFixture {
public:
    void setUp() override {
        RouterCatalogCacheTestFixture::setUp();
        const ShardKeyPattern shardKeyPattern(BSON("a" << 1));
        makeCollectionRoutingInfo(
            kNss, shardKeyPattern, nullptr, false, {BSON("a" << splitPoint)}, {});
        _criTargeter = CollectionRoutingInfoTargeter(operationContext(), kNss);
    }

    OperationContext* getOpCtx() {
        return operationContext();
    }

    CollectionRoutingInfoTargeter getCollectionRoutingInfoTargeter() const {
        return *_criTargeter;
    }

    const TxnNumber kTxnNumber = 5;

private:
    FailPointEnableBlock fp{"isTrackedTimeSeriesBucketsNamespaceAlwaysTrue"};
    boost::optional<CollectionRoutingInfoTargeter> _criTargeter;
};

TEST_F(TimeseriesRetryableUpdateFixture, SingleUpdateWithShardKey) {
    // Sets up for retryable writes.
    getOpCtx()->setLogicalSessionId(makeLogicalSessionIdForTest());
    getOpCtx()->setTxnNumber(kTxnNumber);

    // Do single-target, single doc batch write op.
    BatchedCommandRequest request([&] {
        write_ops::UpdateCommandRequest updateOp(kNss);
        updateOp.setUpdates({buildUpdate(BSON("a" << 1), BSONObj(), false)});
        return updateOp;
    }());

    BatchWriteOp batchOp(getOpCtx(), request);

    std::map<ShardId, std::unique_ptr<TargetedWriteBatch>> targeted;
    auto status = batchOp.targetBatch(getCollectionRoutingInfoTargeter(), false, &targeted);
    ASSERT_OK(status);
    ASSERT_EQUALS(status.getValue(), WriteType::TimeseriesRetryableUpdate);
    ASSERT(!batchOp.isFinished());
    ASSERT_EQUALS(targeted.size(), 1u);
}

TEST_F(TimeseriesRetryableUpdateFixture, SingleUpdateWithoutShardKey) {
    // Sets up for retryable writes.
    getOpCtx()->setLogicalSessionId(makeLogicalSessionIdForTest());
    getOpCtx()->setTxnNumber(kTxnNumber);

    // Do single-target, single doc batch write op.
    BatchedCommandRequest request([&] {
        write_ops::UpdateCommandRequest updateOp(kNss);
        updateOp.setUpdates({buildUpdate(BSON("y" << 1), BSONObj(), false)});
        return updateOp;
    }());

    BatchWriteOp batchOp(getOpCtx(), request);

    std::map<ShardId, std::unique_ptr<TargetedWriteBatch>> targeted;
    auto status = batchOp.targetBatch(getCollectionRoutingInfoTargeter(), false, &targeted);
    ASSERT_OK(status);
    ASSERT_EQUALS(status.getValue(), WriteType::WithoutShardKeyOrId);
    ASSERT(!batchOp.isFinished());
    ASSERT_EQUALS(targeted.size(), 2u);
}

TEST_F(TimeseriesRetryableUpdateFixture, MultipleOrderedUpdateWithShardKey) {
    // Sets up for retryable writes.
    getOpCtx()->setLogicalSessionId(makeLogicalSessionIdForTest());
    getOpCtx()->setTxnNumber(kTxnNumber);

    // Do single-target, multi doc batch write op.
    BatchedCommandRequest request([&] {
        write_ops::UpdateCommandRequest updateOp(kNss);
        updateOp.setWriteCommandRequestBase([] {
            write_ops::WriteCommandRequestBase wcb;
            wcb.setOrdered(true);
            return wcb;
        }());
        updateOp.setUpdates({buildUpdate(BSON("a" << 1), BSONObj(), false),
                             buildUpdate(BSON("a" << 1), BSONObj(), false)});
        return updateOp;
    }());

    BatchWriteOp batchOp(getOpCtx(), request);

    std::map<ShardId, std::unique_ptr<TargetedWriteBatch>> targeted;
    auto status = batchOp.targetBatch(getCollectionRoutingInfoTargeter(), false, &targeted);
    ASSERT_OK(status);
    ASSERT_EQUALS(status.getValue(), WriteType::TimeseriesRetryableUpdate);
    ASSERT(!batchOp.isFinished());
    ASSERT_EQUALS(targeted.size(), 1u);

    BatchedCommandResponse response;
    buildResponse(1, &response);

    // Respond to first targeted batch.
    batchOp.noteBatchResponse(*targeted.begin()->second, response, nullptr);
    ASSERT(!batchOp.isFinished());

    targeted.clear();

    auto status2 = batchOp.targetBatch(getCollectionRoutingInfoTargeter(), false, &targeted);
    ASSERT_OK(status2);
    ASSERT_EQUALS(status2.getValue(), WriteType::TimeseriesRetryableUpdate);
    ASSERT(!batchOp.isFinished());
    ASSERT_EQUALS(targeted.size(), 1u);
    ASSERT_EQUALS(targeted.begin()->second->getWrites().size(), 1u);

    // Respond to second targeted batch.
    batchOp.noteBatchResponse(*targeted.begin()->second, response, nullptr);
    ASSERT(batchOp.isFinished());
}

TEST_F(TimeseriesRetryableUpdateFixture, MultipleUnorderedUpdateWithShardKey) {
    // Sets up for retryable writes.
    getOpCtx()->setLogicalSessionId(makeLogicalSessionIdForTest());
    getOpCtx()->setTxnNumber(kTxnNumber);

    // Do single-target, multi doc batch write op.
    BatchedCommandRequest request([&] {
        write_ops::UpdateCommandRequest updateOp(kNss);
        updateOp.setWriteCommandRequestBase([] {
            write_ops::WriteCommandRequestBase wcb;
            wcb.setOrdered(false);
            return wcb;
        }());
        updateOp.setUpdates({buildUpdate(BSON("a" << 1), BSONObj(), false),
                             buildUpdate(BSON("a" << 1), BSONObj(), false)});
        return updateOp;
    }());

    BatchWriteOp batchOp(getOpCtx(), request);

    std::map<ShardId, std::unique_ptr<TargetedWriteBatch>> targeted;
    auto status = batchOp.targetBatch(getCollectionRoutingInfoTargeter(), false, &targeted);
    ASSERT_OK(status);
    ASSERT_EQUALS(status.getValue(), WriteType::TimeseriesRetryableUpdate);
    ASSERT(!batchOp.isFinished());
    ASSERT_EQUALS(targeted.size(), 1u);

    BatchedCommandResponse response;
    buildResponse(1, &response);

    // Respond to first targeted batch.
    batchOp.noteBatchResponse(*targeted.begin()->second, response, nullptr);
    ASSERT(!batchOp.isFinished());

    targeted.clear();

    auto status2 = batchOp.targetBatch(getCollectionRoutingInfoTargeter(), false, &targeted);
    ASSERT_OK(status2);
    ASSERT_EQUALS(status2.getValue(), WriteType::TimeseriesRetryableUpdate);
    ASSERT(!batchOp.isFinished());
    ASSERT_EQUALS(targeted.size(), 1u);
    ASSERT_EQUALS(targeted.begin()->second->getWrites().size(), 1u);

    // Respond to second targeted batch.
    batchOp.noteBatchResponse(*targeted.begin()->second, response, nullptr);
    ASSERT(batchOp.isFinished());
}

class WriteWithoutShardKeyWithIdFixture : public RouterCatalogCacheTestFixture {
public:
    void setUp() override {
        RouterCatalogCacheTestFixture::setUp();
    }

    OperationContext* getOpCtx() {
        return operationContext();
    }
    CollectionRoutingInfoTargeter getCollectionRoutingInfoTargeter() const {
        return *_criTargeter;
    }

protected:
    boost::optional<CollectionRoutingInfoTargeter> _criTargeter;
    const ShardKeyPattern _shardKeyPattern{BSON("x" << 1)};
};

TEST_F(WriteWithoutShardKeyWithIdFixture, UpdateOneAndDeleteOneSingleShardIsOrdinary) {
    RAIIServerParameterControllerForTest _featureFlagController{
        "featureFlagUpdateOneWithIdWithoutShardKey", true};

    std::vector<BatchedCommandRequest*> requests;

    BatchedCommandRequest updateRequest([&] {
        write_ops::UpdateCommandRequest updateOp(kNss);
        updateOp.setUpdates({buildUpdate(BSON("_id" << 1), BSON("$inc" << BSON("a" << 1)), false)});
        return updateOp;
    }());

    BatchedCommandRequest deleteRequest([&] {
        write_ops::DeleteCommandRequest deleteOp(kNss);
        deleteOp.setDeletes({buildDelete(BSON("_id" << 1), false)});
        return deleteOp;
    }());

    requests.emplace_back(&updateRequest);
    requests.emplace_back(&deleteRequest);

    makeCollectionRoutingInfo(kNss, _shardKeyPattern, nullptr, false, {}, {});
    _criTargeter = CollectionRoutingInfoTargeter(getOpCtx(), kNss);
    for (auto request : requests) {
        BatchWriteOp batchOp(getOpCtx(), *request);

        std::map<ShardId, std::unique_ptr<TargetedWriteBatch>> targeted;
        auto status = batchOp.targetBatch(getCollectionRoutingInfoTargeter(), false, &targeted);
        ASSERT_OK(status);
        ASSERT_EQUALS(status.getValue(), WriteType::Ordinary);
        ASSERT_EQUALS(targeted.size(), 1u);
    }
}

TEST_F(WriteWithoutShardKeyWithIdFixture,
       UpdateOneReplacementStyleIsNotWriteWithoutShardKeyWithId) {
    RAIIServerParameterControllerForTest _featureFlagController{
        "featureFlagUpdateOneWithIdWithoutShardKey", true};

    BatchedCommandRequest request([&] {
        write_ops::UpdateCommandRequest updateOp(kNss);
        // Replacement style update.
        updateOp.setUpdates({buildUpdate(BSON("_id" << 1), BSON("a" << 1), false)});
        return updateOp;
    }());

    BatchWriteOp batchOp(getOpCtx(), request);

    makeCollectionRoutingInfo(kNss, _shardKeyPattern, nullptr, false, {}, {});
    _criTargeter = CollectionRoutingInfoTargeter(getOpCtx(), kNss);

    std::map<ShardId, std::unique_ptr<TargetedWriteBatch>> targeted;
    auto status = batchOp.targetBatch(getCollectionRoutingInfoTargeter(), false, &targeted);
    ASSERT_OK(status);
    ASSERT_NOT_EQUALS(status.getValue(), WriteType::WithoutShardKeyWithId);
}

TEST_F(WriteWithoutShardKeyWithIdFixture, UpdateOneAndDeleteOneBroadcast) {
    RAIIServerParameterControllerForTest _featureFlagController{
        "featureFlagUpdateOneWithIdWithoutShardKey", true};

    std::vector<BatchedCommandRequest*> requests;

    BatchedCommandRequest updateRequest([&] {
        write_ops::UpdateCommandRequest updateOp(kNss);
        // Op style update.
        updateOp.setUpdates({buildUpdate(BSON("_id" << 1), BSON("$inc" << BSON("a" << 1)), false)});
        return updateOp;
    }());

    BatchedCommandRequest deleteRequest([&] {
        write_ops::DeleteCommandRequest deleteOp(kNss);
        deleteOp.setDeletes({buildDelete(BSON("_id" << 1), false)});
        return deleteOp;
    }());

    requests.emplace_back(&updateRequest);
    requests.emplace_back(&deleteRequest);

    makeCollectionRoutingInfo(kNss, _shardKeyPattern, nullptr, false, {BSON("x" << 0)}, {});
    _criTargeter = CollectionRoutingInfoTargeter(getOpCtx(), kNss);

    for (auto request : requests) {
        BatchWriteOp batchOp(getOpCtx(), *request);

        std::map<ShardId, std::unique_ptr<TargetedWriteBatch>> targeted;
        auto status = batchOp.targetBatch(getCollectionRoutingInfoTargeter(), false, &targeted);
        ASSERT_OK(status);
        ASSERT_EQ(status.getValue(), WriteType::WithoutShardKeyWithId);
        ASSERT_EQUALS(targeted.size(), 2);

        BatchedCommandResponse response;
        buildResponse(1, &response);
        auto iterator = targeted.begin();

        // Respond to first targeted batch.
        batchOp.noteBatchResponse(*iterator->second, response, nullptr);
        ASSERT(batchOp.isFinished());
    }
}

TEST_F(WriteWithoutShardKeyWithIdFixture, UpdateOneAndDeleteOneBroadcastWithNoMatch) {
    RAIIServerParameterControllerForTest _featureFlagController{
        "featureFlagUpdateOneWithIdWithoutShardKey", true};

    std::vector<BatchedCommandRequest*> requests;

    BatchedCommandRequest updateRequest([&] {
        write_ops::UpdateCommandRequest updateOp(kNss);
        // Op style update.
        updateOp.setUpdates({buildUpdate(BSON("_id" << 1), BSON("$inc" << BSON("a" << 1)), false)});
        return updateOp;
    }());

    BatchedCommandRequest deleteRequest([&] {
        write_ops::DeleteCommandRequest deleteOp(kNss);
        deleteOp.setDeletes({buildDelete(BSON("_id" << 1), false)});
        return deleteOp;
    }());

    requests.emplace_back(&updateRequest);
    requests.emplace_back(&deleteRequest);

    makeCollectionRoutingInfo(kNss, _shardKeyPattern, nullptr, false, {BSON("x" << 0)}, {});
    _criTargeter = CollectionRoutingInfoTargeter(getOpCtx(), kNss);

    for (auto request : requests) {
        BatchWriteOp batchOp(getOpCtx(), *request);

        std::map<ShardId, std::unique_ptr<TargetedWriteBatch>> targeted;
        auto status = batchOp.targetBatch(getCollectionRoutingInfoTargeter(), false, &targeted);
        ASSERT_OK(status);
        ASSERT_EQ(status.getValue(), WriteType::WithoutShardKeyWithId);
        ASSERT_EQUALS(targeted.size(), 2);

        BatchedCommandResponse firstShardResp;
        buildResponse(0, &firstShardResp);

        auto iterator = targeted.begin();

        // Respond to first targeted batch.
        batchOp.noteBatchResponse(*iterator->second, firstShardResp, nullptr);

        ASSERT(!batchOp.isFinished());
        iterator++;

        BatchedCommandResponse secondShardResp;
        buildResponse(0, &secondShardResp);

        // Respond to second targeted batch.
        batchOp.noteBatchResponse(*iterator->second, secondShardResp, nullptr);

        ASSERT(batchOp.isFinished());
    }
}

TEST_F(WriteWithoutShardKeyWithIdFixture,
       UpdateOneAndDeleteBroadcastNoMatchWithNonRetryableErrors) {
    RAIIServerParameterControllerForTest _featureFlagController{
        "featureFlagUpdateOneWithIdWithoutShardKey", true};

    std::vector<BatchedCommandRequest*> requests;

    BatchedCommandRequest updateRequest([&] {
        write_ops::UpdateCommandRequest updateOp(kNss);
        // Op style update.
        updateOp.setUpdates({buildUpdate(BSON("_id" << 1), BSON("$inc" << BSON("a" << 1)), false)});
        return updateOp;
    }());

    BatchedCommandRequest deleteRequest([&] {
        write_ops::DeleteCommandRequest deleteOp(kNss);
        deleteOp.setDeletes({buildDelete(BSON("_id" << 1), false)});
        return deleteOp;
    }());

    requests.emplace_back(&updateRequest);
    requests.emplace_back(&deleteRequest);

    makeCollectionRoutingInfo(kNss, _shardKeyPattern, nullptr, false, {BSON("x" << 0)}, {});
    _criTargeter = CollectionRoutingInfoTargeter(getOpCtx(), kNss);
    for (auto& request : requests) {
        BatchWriteOp batchOp(getOpCtx(), *request);

        std::map<ShardId, std::unique_ptr<TargetedWriteBatch>> targeted;
        auto status = batchOp.targetBatch(getCollectionRoutingInfoTargeter(), false, &targeted);
        ASSERT_OK(status);
        ASSERT_EQ(status.getValue(), WriteType::WithoutShardKeyWithId);
        ASSERT_EQUALS(targeted.size(), 2);

        BatchedCommandResponse firstShardResp;
        buildErrResponse(ErrorCodes::NotWritablePrimary, "", &firstShardResp);

        auto iterator = targeted.begin();

        // Respond to first targeted batch.
        batchOp.noteBatchResponse(*iterator->second, firstShardResp, nullptr);
        ASSERT(!batchOp.isFinished());
        iterator++;

        BatchedCommandResponse secondShardResp;
        buildResponse(0, &secondShardResp);

        // Respond to second targeted batch.
        batchOp.noteBatchResponse(*iterator->second, secondShardResp, nullptr);

        ASSERT(batchOp.isFinished());
    }
}

TEST_F(WriteWithoutShardKeyWithIdFixture,
       UpdateOneAndDeleteOneBroadcastMatchWithNonRetryableErrors) {
    RAIIServerParameterControllerForTest _featureFlagController{
        "featureFlagUpdateOneWithIdWithoutShardKey", true};

    std::vector<BatchedCommandRequest*> requests;

    BatchedCommandRequest updateRequest([&] {
        write_ops::UpdateCommandRequest updateOp(kNss);
        // Op style update.
        updateOp.setUpdates({buildUpdate(BSON("_id" << 1), BSON("$inc" << BSON("a" << 1)), false)});
        return updateOp;
    }());

    BatchedCommandRequest deleteRequest([&] {
        write_ops::DeleteCommandRequest deleteOp(kNss);
        deleteOp.setDeletes({buildDelete(BSON("_id" << 1), false)});
        return deleteOp;
    }());

    requests.emplace_back(&updateRequest);
    requests.emplace_back(&deleteRequest);

    makeCollectionRoutingInfo(kNss, _shardKeyPattern, nullptr, false, {BSON("x" << 0)}, {});
    _criTargeter = CollectionRoutingInfoTargeter(getOpCtx(), kNss);

    for (auto request : requests) {
        BatchWriteOp batchOp(getOpCtx(), *request);

        std::map<ShardId, std::unique_ptr<TargetedWriteBatch>> targeted;
        auto status = batchOp.targetBatch(getCollectionRoutingInfoTargeter(), false, &targeted);
        ASSERT_OK(status);
        ASSERT_EQ(status.getValue(), WriteType::WithoutShardKeyWithId);
        ASSERT_EQUALS(targeted.size(), 2);

        BatchedCommandResponse firstShardResp;
        buildErrResponse(ErrorCodes::NotWritablePrimary, "", &firstShardResp);

        auto iterator = targeted.begin();

        // Respond to first targeted batch.
        batchOp.noteBatchResponse(*iterator->second, firstShardResp, nullptr);
        ASSERT(!batchOp.isFinished());
        iterator++;

        BatchedCommandResponse secondShardResp;
        buildResponse(1, &secondShardResp);

        // Respond to second targeted batch.
        batchOp.noteBatchResponse(*iterator->second, secondShardResp, nullptr);

        ASSERT(batchOp.isFinished());
    }
}

TEST_F(WriteWithoutShardKeyWithIdFixture,
       UpdateOneAndDeleteOneBroadcastNoMatchWithRetryableErrors) {
    RAIIServerParameterControllerForTest _featureFlagController{
        "featureFlagUpdateOneWithIdWithoutShardKey", true};

    std::vector<BatchedCommandRequest*> requests;

    BatchedCommandRequest updateRequest([&] {
        write_ops::UpdateCommandRequest updateOp(kNss);
        // Op style update.
        updateOp.setUpdates({buildUpdate(BSON("_id" << 1), BSON("$inc" << BSON("a" << 1)), false)});
        return updateOp;
    }());

    BatchedCommandRequest deleteRequest([&] {
        write_ops::DeleteCommandRequest deleteOp(kNss);
        deleteOp.setDeletes({buildDelete(BSON("_id" << 1), false)});
        return deleteOp;
    }());

    requests.emplace_back(&updateRequest);
    requests.emplace_back(&deleteRequest);

    makeCollectionRoutingInfo(kNss, _shardKeyPattern, nullptr, false, {BSON("x" << 0)}, {});
    _criTargeter = CollectionRoutingInfoTargeter(getOpCtx(), kNss);

    for (auto request : requests) {
        BatchWriteOp batchOp(getOpCtx(), *request);

        std::map<ShardId, std::unique_ptr<TargetedWriteBatch>> targeted;
        auto status = batchOp.targetBatch(getCollectionRoutingInfoTargeter(), false, &targeted);
        ASSERT_OK(status);
        ASSERT_EQ(status.getValue(), WriteType::WithoutShardKeyWithId);
        ASSERT_EQUALS(targeted.size(), 2);

        const static OID epoch = OID::gen();
        const static Timestamp timestamp{2};

        BatchedCommandResponse firstShardResp;
        firstShardResp.addToErrDetails(write_ops::WriteError(
            0,
            Status(StaleConfigInfo(
                       kNss,
                       ShardVersionFactory::make(ChunkVersion({epoch, timestamp}, {1, 0}),
                                                 boost::optional<CollectionIndexes>(boost::none)),
                       ShardVersionFactory::make(ChunkVersion({epoch, timestamp}, {2, 0}),
                                                 boost::optional<CollectionIndexes>(boost::none)),
                       ShardId("TestShard")),
                   "Stale error")));
        firstShardResp.setStatus(Status::OK());

        auto iterator = targeted.begin();

        // Respond to first targeted batch.
        TrackedErrors trackedErrors;
        trackedErrors.startTracking(ErrorCodes::StaleConfig);
        batchOp.noteBatchResponse(*iterator->second, firstShardResp, &trackedErrors);

        ASSERT(!batchOp.isFinished());
        iterator++;

        BatchedCommandResponse secondShardResp;
        buildResponse(0, &secondShardResp);

        // Respond to second targeted batch.
        batchOp.noteBatchResponse(*iterator->second, secondShardResp, nullptr);

        ASSERT(!batchOp.isFinished());
    }
}

TEST_F(WriteWithoutShardKeyWithIdFixture, UpdateOneAndDeleteOneBroadcastMatchWithRetryableErrors) {
    RAIIServerParameterControllerForTest _featureFlagController{
        "featureFlagUpdateOneWithIdWithoutShardKey", true};

    std::vector<BatchedCommandRequest*> requests;

    BatchedCommandRequest updateRequest([&] {
        write_ops::UpdateCommandRequest updateOp(kNss);
        // Op style update.
        updateOp.setUpdates({buildUpdate(BSON("_id" << 1), BSON("$inc" << BSON("a" << 1)), false)});
        return updateOp;
    }());

    BatchedCommandRequest deleteRequest([&] {
        write_ops::DeleteCommandRequest deleteOp(kNss);
        deleteOp.setDeletes({buildDelete(BSON("_id" << 1), false)});
        return deleteOp;
    }());

    requests.emplace_back(&updateRequest);
    requests.emplace_back(&deleteRequest);

    makeCollectionRoutingInfo(
        kNss, _shardKeyPattern, nullptr, false, {BSON("x" << 0), BSON("x" << 100)}, {});
    _criTargeter = CollectionRoutingInfoTargeter(getOpCtx(), kNss);

    for (auto request : requests) {
        BatchWriteOp batchOp(getOpCtx(), *request);

        std::map<ShardId, std::unique_ptr<TargetedWriteBatch>> targeted;
        auto status = batchOp.targetBatch(getCollectionRoutingInfoTargeter(), false, &targeted);
        ASSERT_OK(status);
        ASSERT_EQ(status.getValue(), WriteType::WithoutShardKeyWithId);
        ASSERT_EQUALS(targeted.size(), 3);

        const static OID epoch = OID::gen();
        const static Timestamp timestamp{2};

        BatchedCommandResponse firstShardResp;
        firstShardResp.addToErrDetails(write_ops::WriteError(
            0,
            Status(StaleConfigInfo(
                       kNss,
                       ShardVersionFactory::make(ChunkVersion({epoch, timestamp}, {1, 0}),
                                                 boost::optional<CollectionIndexes>(boost::none)),
                       ShardVersionFactory::make(ChunkVersion({epoch, timestamp}, {2, 0}),
                                                 boost::optional<CollectionIndexes>(boost::none)),
                       ShardId("TestShard")),
                   "Stale error")));
        firstShardResp.setStatus(Status::OK());

        auto iterator = targeted.begin();

        // Respond to first targeted batch.
        TrackedErrors trackedErrors;
        trackedErrors.startTracking(ErrorCodes::StaleConfig);
        batchOp.noteBatchResponse(*iterator->second, firstShardResp, &trackedErrors);
        ASSERT(!batchOp.isFinished());
        iterator++;

        BatchedCommandResponse secondShardResp;
        buildResponse(1, &secondShardResp);

        // Respond to second targeted batch.
        batchOp.noteBatchResponse(*iterator->second, secondShardResp, nullptr);

        ASSERT(batchOp.isFinished());
    }
}

TEST_F(WriteWithoutShardKeyWithIdFixture, UpdateOneBroadcastNoMatchWithStaleDBError) {
    RAIIServerParameterControllerForTest _featureFlagController{
        "featureFlagUpdateOneWithIdWithoutShardKey", true};

    BatchedCommandRequest request([&] {
        write_ops::UpdateCommandRequest updateOp(kNss);
        // Op style update.
        updateOp.setUpdates({buildUpdate(BSON("_id" << 1), BSON("$inc" << BSON("a" << 1)), false)});
        return updateOp;
    }());

    makeCollectionRoutingInfo(kNss, _shardKeyPattern, nullptr, false, {BSON("x" << 0)}, {});
    _criTargeter = CollectionRoutingInfoTargeter(getOpCtx(), kNss);

    BatchWriteOp batchOp(getOpCtx(), request);

    std::map<ShardId, std::unique_ptr<TargetedWriteBatch>> targeted;
    auto status = batchOp.targetBatch(getCollectionRoutingInfoTargeter(), false, &targeted);
    ASSERT_OK(status);
    ASSERT_EQ(status.getValue(), WriteType::WithoutShardKeyWithId);
    ASSERT_EQUALS(targeted.size(), 2);

    const static Timestamp timestamp{2};

    BatchedCommandResponse firstShardResp;
    firstShardResp.addToErrDetails(write_ops::WriteError(
        0,
        Status(StaleDbRoutingVersion(kNss.dbName(), DatabaseVersion(), boost::none),
               "Stale DB error")));
    firstShardResp.setStatus(Status::OK());

    auto iterator = targeted.begin();

    // Respond to first targeted batch.
    TrackedErrors trackedErrors;
    trackedErrors.startTracking(ErrorCodes::StaleDbVersion);
    batchOp.noteBatchResponse(*iterator->second, firstShardResp, &trackedErrors);

    ASSERT(!batchOp.isFinished());
    iterator++;

    BatchedCommandResponse secondShardResp;
    buildResponse(0, &secondShardResp);

    // Respond to second targeted batch.
    batchOp.noteBatchResponse(*iterator->second, secondShardResp, nullptr);

    ASSERT(!batchOp.isFinished());
}

TEST_F(WriteWithoutShardKeyWithIdFixture,
       UpdateOrDeleteInTransactionIsNotWriteWithoutShardKeyWithIdWriteType) {
    RAIIServerParameterControllerForTest _featureFlagController{
        "featureFlagUpdateOneWithIdWithoutShardKey", true};

    std::vector<BatchedCommandRequest*> requests;

    BatchedCommandRequest updateRequest([&] {
        write_ops::UpdateCommandRequest updateOp(kNss);
        // Op style update.
        updateOp.setUpdates({buildUpdate(BSON("_id" << 1), BSON("$inc" << BSON("a" << 1)), false)});
        return updateOp;
    }());

    BatchedCommandRequest deleteRequest([&] {
        write_ops::DeleteCommandRequest deleteOp(kNss);
        deleteOp.setDeletes({buildDelete(BSON("_id" << 1), false)});
        return deleteOp;
    }());

    requests.emplace_back(&updateRequest);
    requests.emplace_back(&deleteRequest);

    makeCollectionRoutingInfo(kNss, _shardKeyPattern, nullptr, false, {BSON("x" << 0)}, {});
    _criTargeter = CollectionRoutingInfoTargeter(getOpCtx(), kNss);

    const TxnNumber kTxnNumber = 0;
    getOpCtx()->setLogicalSessionId(makeLogicalSessionIdForTest());
    getOpCtx()->setTxnNumber(kTxnNumber);
    repl::ReadConcernArgs::get(getOpCtx()) =
        repl::ReadConcernArgs(repl::ReadConcernLevel::kSnapshotReadConcern);

    boost::optional<RouterOperationContextSession> _scopedSession(getOpCtx());

    auto txnRouter = TransactionRouter::get(getOpCtx());
    txnRouter.beginOrContinueTxn(
        getOpCtx(), kTxnNumber, TransactionRouter::TransactionActions::kStart);

    for (auto request : requests) {
        BatchWriteOp batchOp(getOpCtx(), *request);

        std::map<ShardId, std::unique_ptr<TargetedWriteBatch>> targeted;
        auto status = batchOp.targetBatch(getCollectionRoutingInfoTargeter(), false, &targeted);
        ASSERT_OK(status);
        ASSERT_NE(status.getValue(), WriteType::WithoutShardKeyWithId);
        // This should still be a broadcast
        ASSERT_EQ(targeted.size(), 2);
    }
    _scopedSession.reset();
}

TEST_F(WriteWithoutShardKeyWithIdFixture, UpdateRetriedAfterWCError) {
    RAIIServerParameterControllerForTest _featureFlagController{
        "featureFlagUpdateOneWithIdWithoutShardKey", true};

    BatchedCommandRequest updateRequest([&] {
        write_ops::UpdateCommandRequest updateOp(kNss);
        // Op style update.
        updateOp.setUpdates({buildUpdate(BSON("_id" << 1), BSON("$inc" << BSON("a" << 1)), false)});
        return updateOp;
    }());

    makeCollectionRoutingInfo(kNss, _shardKeyPattern, nullptr, false, {BSON("x" << 0)}, {});
    _criTargeter = CollectionRoutingInfoTargeter(getOpCtx(), kNss);

    BatchWriteOp batchOp(getOpCtx(), updateRequest);

    std::map<ShardId, std::unique_ptr<TargetedWriteBatch>> targeted;
    auto status = batchOp.targetBatch(getCollectionRoutingInfoTargeter(), false, &targeted);
    ASSERT_OK(status);
    ASSERT_EQ(status.getValue(), WriteType::WithoutShardKeyWithId);
    ASSERT_EQUALS(targeted.size(), 2);

    BatchedCommandResponse firstShardResp;
    addWCError(&firstShardResp);
    firstShardResp.setStatus(Status::OK());

    auto iterator = targeted.begin();

    // Respond to first targeted batch.
    batchOp.noteBatchResponse(*iterator->second, firstShardResp, nullptr);
    ASSERT(!batchOp.isFinished());
    iterator++;

    const static OID epoch = OID::gen();
    const static Timestamp timestamp{2};

    BatchedCommandResponse secondShardResp;
    secondShardResp.setStatus(Status::OK());
    secondShardResp.addToErrDetails(write_ops::WriteError(
        0,
        Status(StaleConfigInfo(
                   kNss,
                   ShardVersionFactory::make(ChunkVersion({epoch, timestamp}, {1, 0}),
                                             boost::optional<CollectionIndexes>(boost::none)),
                   ShardVersionFactory::make(ChunkVersion({epoch, timestamp}, {2, 0}),
                                             boost::optional<CollectionIndexes>(boost::none)),
                   ShardId("TestShard")),
               "Stale error")));

    // Respond to second targeted batch.
    TrackedErrors trackedErrors;
    trackedErrors.startTracking(ErrorCodes::StaleConfig);
    batchOp.noteBatchResponse(*iterator->second, secondShardResp, &trackedErrors);

    // Since this batch would be retried the WCError must be cleared.
    batchOp.handleDeferredWriteConcernErrors();

    ASSERT(!batchOp.isFinished());

    BulkWriteReplyItem bwItem;
    batchOp.getWriteOp(0).setOpComplete(bwItem);

    BatchedCommandResponse clientResponse;
    batchOp.buildClientResponse(&clientResponse);
    ASSERT(clientResponse.getOk());
    ASSERT_EQUALS(clientResponse.getN(), 0);
    ASSERT_FALSE(clientResponse.isWriteConcernErrorSet());
}
}  // namespace
}  // namespace mongo
