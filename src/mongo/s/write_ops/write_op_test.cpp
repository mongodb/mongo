/**
 *    Copyright (C) 2013-2015 MongoDB Inc.
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

#include "mongo/base/owned_pointer_vector.h"
#include "mongo/db/operation_context_noop.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/mock_ns_targeter.h"
#include "mongo/s/write_ops/write_error_detail.h"
#include "mongo/s/write_ops/write_op.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

WriteErrorDetail buildError(int code, const BSONObj& info, const std::string& message) {
    WriteErrorDetail error;
    error.setErrCode(code);
    error.setErrInfo(info);
    error.setErrMessage(message);

    return error;
}

write_ops::DeleteOpEntry buildDelete(const BSONObj& query, bool multi) {
    write_ops::DeleteOpEntry entry;
    entry.setQ(query);
    entry.setMulti(multi);
    return entry;
}

struct EndpointComp {
    bool operator()(const TargetedWrite* writeA, const TargetedWrite* writeB) const {
        return writeA->endpoint.shardName.compare(writeB->endpoint.shardName) < 0;
    }
};

void sortByEndpoint(std::vector<TargetedWrite*>* writes) {
    std::sort(writes->begin(), writes->end(), EndpointComp());
}

// Test of basic error-setting on write op
TEST(WriteOpTests, BasicError) {
    BatchedCommandRequest request([&] {
        write_ops::Insert insertOp(NamespaceString("foo.bar"));
        insertOp.setDocuments({BSON("x" << 1)});
        return insertOp;
    }());

    WriteOp writeOp(BatchItemRef(&request, 0));
    ASSERT_EQUALS(writeOp.getWriteState(), WriteOpState_Ready);

    const auto error(buildError(ErrorCodes::UnknownError, BSON("data" << 12345), "some message"));

    writeOp.setOpError(error);
    ASSERT_EQUALS(writeOp.getWriteState(), WriteOpState_Error);
    ASSERT_EQUALS(writeOp.getOpError().getErrCode(), error.getErrCode());
    ASSERT_EQUALS(writeOp.getOpError().getErrInfo()["data"].Int(),
                  error.getErrInfo()["data"].Int());
    ASSERT_EQUALS(writeOp.getOpError().getErrMessage(), error.getErrMessage());
}

TEST(WriteOpTests, TargetSingle) {
    OperationContextNoop opCtx;

    NamespaceString nss("foo.bar");
    ShardEndpoint endpoint(ShardId("shard"), ChunkVersion::IGNORED());

    BatchedCommandRequest request([&] {
        write_ops::Insert insertOp(nss);
        insertOp.setDocuments({BSON("x" << 1)});
        return insertOp;
    }());

    // Do single-target write op

    WriteOp writeOp(BatchItemRef(&request, 0));
    ASSERT_EQUALS(writeOp.getWriteState(), WriteOpState_Ready);

    MockNSTargeter targeter;
    targeter.init(nss, {MockRange(endpoint, BSON("x" << MINKEY), BSON("x" << MAXKEY))});

    OwnedPointerVector<TargetedWrite> targetedOwned;
    std::vector<TargetedWrite*>& targeted = targetedOwned.mutableVector();
    Status status = writeOp.targetWrites(&opCtx, targeter, &targeted);

    ASSERT(status.isOK());
    ASSERT_EQUALS(writeOp.getWriteState(), WriteOpState_Pending);
    ASSERT_EQUALS(targeted.size(), 1u);
    assertEndpointsEqual(targeted.front()->endpoint, endpoint);

    writeOp.noteWriteComplete(*targeted.front());
    ASSERT_EQUALS(writeOp.getWriteState(), WriteOpState_Completed);
}

// Multi-write targeting test where our query goes to one shard
TEST(WriteOpTests, TargetMultiOneShard) {
    OperationContextNoop opCtx;

    NamespaceString nss("foo.bar");
    ShardEndpoint endpointA(ShardId("shardA"), ChunkVersion(10, 0, OID()));
    ShardEndpoint endpointB(ShardId("shardB"), ChunkVersion(20, 0, OID()));
    ShardEndpoint endpointC(ShardId("shardB"), ChunkVersion(20, 0, OID()));

    BatchedCommandRequest request([&] {
        write_ops::Delete deleteOp(nss);
        // Only hits first shard
        deleteOp.setDeletes({buildDelete(BSON("x" << GTE << -2 << LT << -1), false)});
        return deleteOp;
    }());

    WriteOp writeOp(BatchItemRef(&request, 0));
    ASSERT_EQUALS(writeOp.getWriteState(), WriteOpState_Ready);

    MockNSTargeter targeter;
    targeter.init(nss,
                  {MockRange(endpointA, BSON("x" << MINKEY), BSON("x" << 0)),
                   MockRange(endpointB, BSON("x" << 0), BSON("x" << 10)),
                   MockRange(endpointC, BSON("x" << 10), BSON("x" << MAXKEY))});

    OwnedPointerVector<TargetedWrite> targetedOwned;
    std::vector<TargetedWrite*>& targeted = targetedOwned.mutableVector();
    Status status = writeOp.targetWrites(&opCtx, targeter, &targeted);

    ASSERT(status.isOK());
    ASSERT_EQUALS(writeOp.getWriteState(), WriteOpState_Pending);
    ASSERT_EQUALS(targeted.size(), 1u);
    assertEndpointsEqual(targeted.front()->endpoint, endpointA);

    writeOp.noteWriteComplete(*targeted.front());

    ASSERT_EQUALS(writeOp.getWriteState(), WriteOpState_Completed);
}

// Multi-write targeting test where our write goes to more than one shard
TEST(WriteOpTests, TargetMultiAllShards) {
    OperationContextNoop opCtx;

    NamespaceString nss("foo.bar");
    ShardEndpoint endpointA(ShardId("shardA"), ChunkVersion(10, 0, OID()));
    ShardEndpoint endpointB(ShardId("shardB"), ChunkVersion(20, 0, OID()));
    ShardEndpoint endpointC(ShardId("shardB"), ChunkVersion(20, 0, OID()));

    BatchedCommandRequest request([&] {
        write_ops::Delete deleteOp(nss);
        deleteOp.setDeletes({buildDelete(BSON("x" << GTE << -1 << LT << 1), false)});
        return deleteOp;
    }());

    // Do multi-target write op
    WriteOp writeOp(BatchItemRef(&request, 0));
    ASSERT_EQUALS(writeOp.getWriteState(), WriteOpState_Ready);

    MockNSTargeter targeter;
    targeter.init(nss,
                  {MockRange(endpointA, BSON("x" << MINKEY), BSON("x" << 0)),
                   MockRange(endpointB, BSON("x" << 0), BSON("x" << 10)),
                   MockRange(endpointC, BSON("x" << 10), BSON("x" << MAXKEY))});

    OwnedPointerVector<TargetedWrite> targetedOwned;
    std::vector<TargetedWrite*>& targeted = targetedOwned.mutableVector();
    Status status = writeOp.targetWrites(&opCtx, targeter, &targeted);

    ASSERT(status.isOK());
    ASSERT_EQUALS(writeOp.getWriteState(), WriteOpState_Pending);
    ASSERT_EQUALS(targeted.size(), 3u);
    sortByEndpoint(&targeted);
    ASSERT_EQUALS(targeted[0]->endpoint.shardName, endpointA.shardName);
    ASSERT(ChunkVersion::isIgnoredVersion(targeted[0]->endpoint.shardVersion));
    ASSERT_EQUALS(targeted[1]->endpoint.shardName, endpointB.shardName);
    ASSERT(ChunkVersion::isIgnoredVersion(targeted[1]->endpoint.shardVersion));
    ASSERT_EQUALS(targeted[2]->endpoint.shardName, endpointC.shardName);
    ASSERT(ChunkVersion::isIgnoredVersion(targeted[2]->endpoint.shardVersion));

    writeOp.noteWriteComplete(*targeted[0]);
    writeOp.noteWriteComplete(*targeted[1]);
    writeOp.noteWriteComplete(*targeted[2]);

    ASSERT_EQUALS(writeOp.getWriteState(), WriteOpState_Completed);
}

// Single error after targeting test
TEST(WriteOpTests, ErrorSingle) {
    OperationContextNoop opCtx;

    NamespaceString nss("foo.bar");
    ShardEndpoint endpoint(ShardId("shard"), ChunkVersion::IGNORED());

    BatchedCommandRequest request([&] {
        write_ops::Insert insertOp(nss);
        insertOp.setDocuments({BSON("x" << 1)});
        return insertOp;
    }());

    // Do single-target write op

    WriteOp writeOp(BatchItemRef(&request, 0));
    ASSERT_EQUALS(writeOp.getWriteState(), WriteOpState_Ready);

    MockNSTargeter targeter;
    targeter.init(nss, {MockRange(endpoint, BSON("x" << MINKEY), BSON("x" << MAXKEY))});

    OwnedPointerVector<TargetedWrite> targetedOwned;
    std::vector<TargetedWrite*>& targeted = targetedOwned.mutableVector();
    Status status = writeOp.targetWrites(&opCtx, targeter, &targeted);

    ASSERT(status.isOK());
    ASSERT_EQUALS(writeOp.getWriteState(), WriteOpState_Pending);
    ASSERT_EQUALS(targeted.size(), 1u);
    assertEndpointsEqual(targeted.front()->endpoint, endpoint);

    const auto error(buildError(ErrorCodes::UnknownError, BSON("data" << 12345), "some message"));

    writeOp.noteWriteError(*targeted.front(), error);

    ASSERT_EQUALS(writeOp.getWriteState(), WriteOpState_Error);
    ASSERT_EQUALS(writeOp.getOpError().getErrCode(), error.getErrCode());
    ASSERT_EQUALS(writeOp.getOpError().getErrInfo()["data"].Int(),
                  error.getErrInfo()["data"].Int());
    ASSERT_EQUALS(writeOp.getOpError().getErrMessage(), error.getErrMessage());
}

// Cancel single targeting test
TEST(WriteOpTests, CancelSingle) {
    OperationContextNoop opCtx;

    NamespaceString nss("foo.bar");
    ShardEndpoint endpoint(ShardId("shard"), ChunkVersion::IGNORED());

    BatchedCommandRequest request([&] {
        write_ops::Insert insertOp(nss);
        insertOp.setDocuments({BSON("x" << 1)});
        return insertOp;
    }());

    // Do single-target write op

    WriteOp writeOp(BatchItemRef(&request, 0));
    ASSERT_EQUALS(writeOp.getWriteState(), WriteOpState_Ready);

    MockNSTargeter targeter;
    targeter.init(nss, {MockRange(endpoint, BSON("x" << MINKEY), BSON("x" << MAXKEY))});

    OwnedPointerVector<TargetedWrite> targetedOwned;
    std::vector<TargetedWrite*>& targeted = targetedOwned.mutableVector();
    Status status = writeOp.targetWrites(&opCtx, targeter, &targeted);

    ASSERT(status.isOK());
    ASSERT_EQUALS(writeOp.getWriteState(), WriteOpState_Pending);
    ASSERT_EQUALS(targeted.size(), 1u);
    assertEndpointsEqual(targeted.front()->endpoint, endpoint);

    writeOp.cancelWrites(NULL);

    ASSERT_EQUALS(writeOp.getWriteState(), WriteOpState_Ready);
}

//
// Test retryable errors
//

// Retry single targeting test
TEST(WriteOpTests, RetrySingleOp) {
    OperationContextNoop opCtx;

    NamespaceString nss("foo.bar");
    ShardEndpoint endpoint(ShardId("shard"), ChunkVersion::IGNORED());

    BatchedCommandRequest request([&] {
        write_ops::Insert insertOp(nss);
        insertOp.setDocuments({BSON("x" << 1)});
        return insertOp;
    }());

    // Do single-target write op

    WriteOp writeOp(BatchItemRef(&request, 0));
    ASSERT_EQUALS(writeOp.getWriteState(), WriteOpState_Ready);

    MockNSTargeter targeter;
    targeter.init(nss, {MockRange(endpoint, BSON("x" << MINKEY), BSON("x" << MAXKEY))});

    OwnedPointerVector<TargetedWrite> targetedOwned;
    std::vector<TargetedWrite*>& targeted = targetedOwned.mutableVector();
    Status status = writeOp.targetWrites(&opCtx, targeter, &targeted);

    ASSERT(status.isOK());
    ASSERT_EQUALS(writeOp.getWriteState(), WriteOpState_Pending);
    ASSERT_EQUALS(targeted.size(), 1u);
    assertEndpointsEqual(targeted.front()->endpoint, endpoint);

    // Stale exception
    const auto error(
        buildError(ErrorCodes::StaleShardVersion, BSON("data" << 12345), "some message"));
    writeOp.noteWriteError(*targeted.front(), error);

    ASSERT_EQUALS(writeOp.getWriteState(), WriteOpState_Ready);
}

}  // namespace
}  // namespace mongo
