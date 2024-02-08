/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include <memory>
#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer/batched_write_context.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/tenant_id.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/decorable.h"
#include "mongo/util/uuid.h"

namespace mongo {
namespace {

// This test fixture provides access to a properly initialized global service context to test the
// BatchedWriteContext class and its interaction with WriteUnitOfWork. For batched write
// interactions with the oplog, see BatchedWriteOutputsTest.
class BatchedWriteContextTest : public ServiceContextMongoDTest {};

TEST_F(BatchedWriteContextTest, TestBatchingCondition) {
    auto opCtxRaii = makeOperationContext();
    auto opCtx = opCtxRaii.get();

    auto& bwc = BatchedWriteContext::get(opCtx);
    ASSERT(!bwc.writesAreBatched());
    bwc.setWritesAreBatched(true);
    ASSERT(bwc.writesAreBatched());
    bwc.setWritesAreBatched(false);
    ASSERT(!bwc.writesAreBatched());
}

DEATH_TEST_REGEX_F(BatchedWriteContextTest,
                   TestDoesNotSupportAddingBatchedOperationWhileWritesAreNotBatched,
                   "Invariant failure.*_batchWrites") {
    auto opCtxRaii = makeOperationContext();
    auto opCtx = opCtxRaii.get();

    WriteUnitOfWork wuow(opCtx, WriteUnitOfWork::kGroupForTransaction);
    auto& bwc = BatchedWriteContext::get(opCtx);
    ASSERT(!bwc.writesAreBatched());

    const NamespaceString nss =
        NamespaceString::createNamespaceString_forTest(boost::none, "test", "coll");
    auto op = repl::MutableOplogEntry::makeDeleteOperation(nss, UUID::gen(), BSON("_id" << 0));
    bwc.addBatchedOperation(opCtx, op);
}

DEATH_TEST_REGEX_F(BatchedWriteContextTest,
                   TestDoesNotSupportAddingBatchedOperationOutsideOfWUOW,
                   "Invariant failure.*inAWriteUnitOfWork()") {
    auto opCtxRaii = makeOperationContext();
    auto opCtx = opCtxRaii.get();

    auto& bwc = BatchedWriteContext::get(opCtx);
    // Need to explicitly set writes are batched to simulate op observer starting batched write.
    bwc.setWritesAreBatched(true);

    const NamespaceString nss =
        NamespaceString::createNamespaceString_forTest(boost::none, "test", "coll");
    auto op = repl::MutableOplogEntry::makeDeleteOperation(nss, UUID::gen(), BSON("_id" << 0));
    bwc.addBatchedOperation(opCtx, op);
}

DEATH_TEST_REGEX_F(BatchedWriteContextTest,
                   TestCannotGroupDDLOperation,
                   "Invariant failure.*getOpType.*repl::OpTypeEnum::kDelete.*kInsert.*kUpdate") {
    auto opCtxRaii = makeOperationContext();
    auto opCtx = opCtxRaii.get();

    WriteUnitOfWork wuow(opCtx, WriteUnitOfWork::kGroupForTransaction);
    auto& bwc = BatchedWriteContext::get(opCtx);
    // Need to explicitly set writes are batched to simulate op observer starting batched write.
    bwc.setWritesAreBatched(true);

    const NamespaceString nss =
        NamespaceString::createNamespaceString_forTest(boost::none, "other", "coll");
    auto op = repl::MutableOplogEntry::makeCreateCommand(nss, CollectionOptions(), BSON("v" << 2));
    bwc.addBatchedOperation(opCtx, op);
}

DEATH_TEST_REGEX_F(BatchedWriteContextTest,
                   TestDoesNotSupportPreImagesInCollection,
                   "Invariant "
                   "failure.*getChangeStreamPreImageRecordingMode.*repl::ReplOperation::"
                   "ChangeStreamPreImageRecordingMode::kOff") {
    auto opCtxRaii = makeOperationContext();
    auto opCtx = opCtxRaii.get();

    WriteUnitOfWork wuow(opCtx, WriteUnitOfWork::kGroupForTransaction);
    auto& bwc = BatchedWriteContext::get(opCtx);
    // Need to explicitly set writes are batched to simulate op observer starting batched write.
    bwc.setWritesAreBatched(true);

    const NamespaceString nss =
        NamespaceString::createNamespaceString_forTest(boost::none, "test", "coll");
    auto op = repl::MutableOplogEntry::makeDeleteOperation(nss, UUID::gen(), BSON("_id" << 0));
    op.setChangeStreamPreImageRecordingMode(
        repl::ReplOperation::ChangeStreamPreImageRecordingMode::kPreImagesCollection);
    bwc.addBatchedOperation(opCtx, op);
}

DEATH_TEST_REGEX_F(BatchedWriteContextTest,
                   TestDoesNotSupportMultiDocTxn,
                   "Invariant failure.*!opCtx->inMultiDocumentTransaction()") {
    auto opCtxRaii = makeOperationContext();
    auto opCtx = opCtxRaii.get();
    opCtx->setInMultiDocumentTransaction();

    WriteUnitOfWork wuow(opCtx, WriteUnitOfWork::kGroupForTransaction);
    auto& bwc = BatchedWriteContext::get(opCtx);
    // Need to explicitly set writes are batched to simulate op observer starting batched write.
    bwc.setWritesAreBatched(true);

    const NamespaceString nss =
        NamespaceString::createNamespaceString_forTest(boost::none, "test", "coll");
    auto op = repl::MutableOplogEntry::makeDeleteOperation(nss, UUID::gen(), BSON("_id" << 0));
    bwc.addBatchedOperation(opCtx, op);
}

TEST_F(BatchedWriteContextTest, TestAcceptedBatchOperationsSucceeds) {
    auto opCtxRaii = makeOperationContext();
    auto opCtx = opCtxRaii.get();
    auto& bwc = BatchedWriteContext::get(opCtx);

    WriteUnitOfWork wuow(opCtx, WriteUnitOfWork::kGroupForTransaction);
    // Need to explicitly set writes are batched to simulate op observer
    bwc.setWritesAreBatched(true);

    BatchedWriteContext::BatchedOperations* ops = bwc.getBatchedOperations(opCtx);
    ASSERT(ops->isEmpty());

    const NamespaceString nss =
        NamespaceString::createNamespaceString_forTest(boost::none, "test", "coll");
    auto op = repl::MutableOplogEntry::makeDeleteOperation(nss, UUID::gen(), BSON("_id" << 0));
    bwc.addBatchedOperation(opCtx, op);
    ASSERT_FALSE(ops->isEmpty());
    ASSERT_EQ(ops->numOperations(), 1U);

    op = repl::MutableOplogEntry::makeInsertOperation(
        nss, UUID::gen(), BSON("a" << 0), BSON("_id" << 1));
    bwc.addBatchedOperation(opCtx, op);
    ASSERT_EQ(ops->numOperations(), 2U);

    op = repl::MutableOplogEntry::makeInsertOperation(
        nss, UUID::gen(), BSON("a" << 1), BSON("_id" << 2));
    bwc.addBatchedOperation(opCtx, op);
    ASSERT_EQ(ops->numOperations(), 3U);

    // Batched write committing is handled outside of the batched write context and involves the
    // oplog so it is not necessary to commit the WriteUnitOfWork in this test.
    bwc.clearBatchedOperations(opCtx);
    ASSERT(ops->isEmpty());
}
}  // namespace
}  // namespace mongo
