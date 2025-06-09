/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/s/write_ops/unified_write_executor/write_op_producer.h"

#include "mongo/bson/json.h"
#include "mongo/logv2/log.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/unittest/unittest.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace unified_write_executor {
namespace {

static const NamespaceString kNss = NamespaceString::createNamespaceString_forTest("test", "coll");

BulkWriteCommandRequest createBulkWriteInsertsRequest(const NamespaceString& nss,
                                                      size_t numOperations) {
    std::vector<std::variant<BulkWriteInsertOp, BulkWriteUpdateOp, BulkWriteDeleteOp>> operations;
    for (size_t i = 0; i < numOperations; ++i) {
        operations.emplace_back(BulkWriteInsertOp(0, BSONObj()));
    }
    std::vector<NamespaceInfoEntry> nsInfoEntries = {NamespaceInfoEntry(nss)};
    BulkWriteCommandRequest request(std::move(operations), std::move(nsInfoEntries));
    return request;
}

std::shared_ptr<BatchedCommandRequest> createInsertRequest(const NamespaceString& nss) {
    write_ops::InsertCommandRequest insertOp(nss);
    insertOp.setWriteCommandRequestBase([] {
        write_ops::WriteCommandRequestBase wcb;
        wcb.setOrdered(false);
        return wcb;
    }());
    insertOp.setDocuments({BSON("x" << 0), BSON("x" << 1), BSON("x" << 2)});

    return std::make_shared<BatchedCommandRequest>(std::move(insertOp));
}

std::shared_ptr<BatchedCommandRequest> createUpdateRequest(const NamespaceString& nss) {
    auto buildUpdate = [](const BSONObj& query, const BSONObj& updateExpr, bool multi) {
        write_ops::UpdateOpEntry entry;
        entry.setQ(query);
        entry.setU(write_ops::UpdateModification::parseFromClassicUpdate(updateExpr));
        entry.setMulti(multi);
        return entry;
    };
    write_ops::UpdateCommandRequest updateOp(nss);
    updateOp.setUpdates({buildUpdate(BSON("_id" << 1), BSON("$inc" << BSON("a" << 1)), false),
                         buildUpdate(BSON("_id" << 2), BSON("$inc" << BSON("a" << 2)), false),
                         buildUpdate(BSON("_id" << 3), BSON("$inc" << BSON("a" << 3)), false)});
    updateOp.setOrdered(false);

    return std::make_shared<BatchedCommandRequest>(std::move(updateOp));
}

std::shared_ptr<BatchedCommandRequest> createDeleteRequest(const NamespaceString& nss) {
    auto buildDelete = [](const BSONObj& query, bool multi) {
        write_ops::DeleteOpEntry entry;
        entry.setQ(query);
        entry.setMulti(multi);
        return entry;
    };
    write_ops::DeleteCommandRequest deleteOp(nss);
    deleteOp.setDeletes({buildDelete(BSON("x" << GTE << -1 << LT << 2), true),
                         buildDelete(BSON("x" << GTE << -2 << LT << 1), true),
                         buildDelete(BSON("x" << GTE << -3 << LT << 0), true)});

    return std::make_shared<BatchedCommandRequest>(std::move(deleteOp));
}

std::string getBatchTypeString(BatchedCommandRequest::BatchType batchType) {
    switch (batchType) {
        case BatchedCommandRequest::BatchType_Insert:
            return "BatchType_Insert";
        case BatchedCommandRequest::BatchType_Update:
            return "BatchType_Update";
        case BatchedCommandRequest::BatchType_Delete:
            return "BatchType_Delete";
        default:
            return "Unknown batch type";
    }
}

TEST(UnifiedWriteExecutorProducerTest, BulkWriteOpProducerPeekAndAdvance) {
    auto request = createBulkWriteInsertsRequest(kNss, 3);
    MultiWriteOpProducer<BulkWriteCommandRequest> producer(request);

    // Repeated peek and advance three times.
    auto op0 = producer.peekNext();
    ASSERT_TRUE(op0.has_value());
    ASSERT_EQ(op0->getId(), 0);
    producer.advance();

    auto op1 = producer.peekNext();
    ASSERT_TRUE(op1.has_value());
    ASSERT_EQ(op1->getId(), 1);
    producer.advance();

    auto op2 = producer.peekNext();
    ASSERT_TRUE(op2.has_value());
    ASSERT_EQ(op2->getId(), 2);
    producer.advance();

    auto noop = producer.peekNext();
    ASSERT_FALSE(noop.has_value());
}

TEST(UnifiedWriteExecutorProducerTest, BulkWriteOpProducerRepeatedPeek) {
    auto request = createBulkWriteInsertsRequest(kNss, 2);
    MultiWriteOpProducer<BulkWriteCommandRequest> producer(request);

    // Peek the same write op three times then advance.
    auto op0First = producer.peekNext();
    ASSERT_TRUE(op0First.has_value());
    ASSERT_EQ(op0First->getId(), 0);

    auto op0Second = producer.peekNext();
    ASSERT_TRUE(op0Second.has_value());
    ASSERT_EQ(op0Second->getId(), 0);

    auto op0Third = producer.peekNext();
    ASSERT_TRUE(op0Third.has_value());
    ASSERT_EQ(op0Third->getId(), 0);

    producer.advance();

    auto op1Second = producer.peekNext();
    ASSERT_TRUE(op1Second.has_value());
    ASSERT_EQ(op1Second->getId(), 1);
    producer.advance();

    auto noop = producer.peekNext();
    ASSERT_FALSE(noop.has_value());
}

TEST(UnifiedWriteExecutorProducerTest, WriteOpProducerRepeatedAdvance) {
    auto request = createBulkWriteInsertsRequest(kNss, 3);
    MultiWriteOpProducer<BulkWriteCommandRequest> producer(request);

    auto op0First = producer.peekNext();
    ASSERT_TRUE(op0First.has_value());
    ASSERT_EQ(op0First->getId(), 0);

    auto op0Second = producer.peekNext();
    ASSERT_TRUE(op0Second.has_value());
    ASSERT_EQ(op0Second->getId(), 0);

    // Advance twice to skip a write op.
    producer.advance();
    producer.advance();

    auto op2 = producer.peekNext();
    ASSERT_TRUE(op2.has_value());
    ASSERT_EQ(op2->getId(), 2);
    producer.advance();

    auto noop = producer.peekNext();
    ASSERT_FALSE(noop.has_value());
}

TEST(UnifiedWriteExecutorProducerTest, WriteOpProducerAdvancePastEnd) {
    auto request = createBulkWriteInsertsRequest(kNss, 1);
    MultiWriteOpProducer<BulkWriteCommandRequest> producer(request);

    auto op0 = producer.peekNext();
    ASSERT_TRUE(op0.has_value());
    ASSERT_EQ(op0->getId(), 0);
    producer.advance();

    // Advance at the end keeps returning noop.
    auto noopFirst = producer.peekNext();
    ASSERT_FALSE(noopFirst.has_value());
    producer.advance();

    auto noopSecond = producer.peekNext();
    ASSERT_FALSE(noopSecond.has_value());
}

TEST(UnifiedWriteExecutorProducerTest, WriteOpProducerReprocessSingle) {
    auto request = createBulkWriteInsertsRequest(kNss, 3);
    MultiWriteOpProducer<BulkWriteCommandRequest> producer(request);

    auto op0First = producer.peekNext();
    ASSERT_TRUE(op0First.has_value());
    ASSERT_EQ(op0First->getId(), 0);
    producer.advance();

    auto op1 = producer.peekNext();
    ASSERT_TRUE(op1.has_value());
    ASSERT_EQ(op1->getId(), 1);
    producer.advance();

    producer.markOpReprocess(*op0First);

    // Reprocess op0 generates op0 again.
    auto op0Second = producer.peekNext();
    ASSERT_TRUE(op0Second.has_value());
    ASSERT_EQ(op0Second->getId(), 0);
    producer.advance();

    // The op1 is not reprocessed so will not be generated again.
    auto op2 = producer.peekNext();
    ASSERT_TRUE(op2.has_value());
    ASSERT_EQ(op2->getId(), 2);
    producer.advance();

    auto noop = producer.peekNext();
    ASSERT_FALSE(noop.has_value());
}

TEST(UnifiedWriteExecutorProducerTest, WriteOpProducerReprocessLowThenHighId) {
    auto request = createBulkWriteInsertsRequest(kNss, 3);
    MultiWriteOpProducer<BulkWriteCommandRequest> producer(request);

    auto op0First = producer.peekNext();
    ASSERT_TRUE(op0First.has_value());
    ASSERT_EQ(op0First->getId(), 0);
    producer.advance();

    auto op1 = producer.peekNext();
    ASSERT_TRUE(op1.has_value());
    ASSERT_EQ(op1->getId(), 1);
    producer.advance();

    auto op2First = producer.peekNext();
    ASSERT_TRUE(op2First.has_value());
    ASSERT_EQ(op2First->getId(), 2);
    producer.advance();

    auto noopFirst = producer.peekNext();
    ASSERT_FALSE(noopFirst.has_value());

    // Reprocessing multiple ops with low id before high id.
    producer.markOpReprocess(*op0First);
    producer.markOpReprocess(*op2First);

    auto op0Second = producer.peekNext();
    ASSERT_TRUE(op0Second.has_value());
    ASSERT_EQ(op0Second->getId(), 0);
    producer.advance();

    // The op1 between two reprocessed ops will not be generated.
    auto op2Second = producer.peekNext();
    ASSERT_TRUE(op2Second.has_value());
    ASSERT_EQ(op2Second->getId(), 2);
    producer.advance();

    auto noopSecond = producer.peekNext();
    ASSERT_FALSE(noopSecond.has_value());
}

TEST(UnifiedWriteExecutorProducerTest, WriteOpProducerReprocessHighThenLowId) {
    auto request = createBulkWriteInsertsRequest(kNss, 3);
    MultiWriteOpProducer<BulkWriteCommandRequest> producer(request);

    auto op0First = producer.peekNext();
    ASSERT_TRUE(op0First.has_value());
    ASSERT_EQ(op0First->getId(), 0);
    producer.advance();

    auto op1 = producer.peekNext();
    ASSERT_TRUE(op1.has_value());
    ASSERT_EQ(op1->getId(), 1);
    producer.advance();

    auto op2First = producer.peekNext();
    ASSERT_TRUE(op2First.has_value());
    ASSERT_EQ(op2First->getId(), 2);
    producer.advance();

    auto noopFirst = producer.peekNext();
    ASSERT_FALSE(noopFirst.has_value());

    // Reprocessing multiple ops with high id before low id.
    producer.markOpReprocess(*op2First);
    producer.markOpReprocess(*op0First);

    auto op0Second = producer.peekNext();
    ASSERT_TRUE(op0Second.has_value());
    ASSERT_EQ(op0Second->getId(), 0);
    producer.advance();

    // The op1 between two reprocessed ops will not be generated.
    auto op2Second = producer.peekNext();
    ASSERT_TRUE(op2Second.has_value());
    ASSERT_EQ(op2Second->getId(), 2);
    producer.advance();

    auto noopSecond = producer.peekNext();
    ASSERT_FALSE(noopSecond.has_value());
}

TEST(UnifiedWriteExecutorProducerTest, WriteOpProducerReprocessSameOpAgain) {
    auto request = createBulkWriteInsertsRequest(kNss, 3);
    MultiWriteOpProducer<BulkWriteCommandRequest> producer(request);

    auto op0First = producer.peekNext();
    ASSERT_TRUE(op0First.has_value());
    ASSERT_EQ(op0First->getId(), 0);
    producer.advance();

    auto op1 = producer.peekNext();
    ASSERT_TRUE(op1.has_value());
    ASSERT_EQ(op1->getId(), 1);
    producer.advance();

    // Reprocess op0 the first time.
    producer.markOpReprocess(*op0First);

    auto op0Second = producer.peekNext();
    ASSERT_TRUE(op0Second.has_value());
    ASSERT_EQ(op0Second->getId(), 0);
    producer.advance();

    auto op2 = producer.peekNext();
    ASSERT_TRUE(op2.has_value());
    ASSERT_EQ(op2->getId(), 2);
    producer.advance();

    // Reprocess op0 the second time still generates op0 again.
    producer.markOpReprocess(*op0Second);

    auto op0Third = producer.peekNext();
    ASSERT_TRUE(op0Third.has_value());
    ASSERT_EQ(op0Third->getId(), 0);
    producer.advance();

    auto noop = producer.peekNext();
    ASSERT_FALSE(noop.has_value());
}

TEST(UnifiedWriteExecutorProducerTest, BulkWriteOpProducerDifferentNamespaces) {
    const NamespaceString nss0 = NamespaceString::createNamespaceString_forTest("test", "coll0");
    const NamespaceString nss1 = NamespaceString::createNamespaceString_forTest("test", "coll1");
    const NamespaceString nss2 = NamespaceString::createNamespaceString_forTest("test", "coll2");
    BulkWriteCommandRequest request(
        {BulkWriteInsertOp(0, BSON("a" << 0)),
         BulkWriteUpdateOp(1,
                           BSON("a" << 1),
                           write_ops::UpdateModification::parseFromBSON(
                               BSON("$set" << BSON("b" << 1)).firstElement())),
         BulkWriteDeleteOp(2, BSON("a" << 2))},
        {NamespaceInfoEntry(nss0), NamespaceInfoEntry(nss1), NamespaceInfoEntry(nss2)});
    MultiWriteOpProducer<BulkWriteCommandRequest> producer(request);

    // Check ops with different namespaces and request contents.
    auto op0 = producer.peekNext();
    ASSERT_TRUE(op0.has_value());
    ASSERT_EQ(op0->getId(), 0);
    ASSERT_EQ(op0->getNss(), nss0);
    ASSERT_BSONOBJ_EQ(op0->getRef().getDocument(), BSON("a" << 0));
    producer.advance();

    auto op1 = producer.peekNext();
    ASSERT_TRUE(op1.has_value());
    ASSERT_EQ(op1->getId(), 1);
    ASSERT_EQ(op1->getNss(), nss1);
    ASSERT_BSONOBJ_EQ(op1->getRef().getUpdateRef().getFilter(), BSON("a" << 1));
    producer.advance();

    auto op2 = producer.peekNext();
    ASSERT_TRUE(op2.has_value());
    ASSERT_EQ(op2->getId(), 2);
    ASSERT_EQ(op2->getNss(), nss2);
    ASSERT_BSONOBJ_EQ(op2->getRef().getDeleteRef().getFilter(), BSON("a" << 2));
    producer.advance();

    auto noop = producer.peekNext();
    ASSERT_FALSE(noop.has_value());
}

static const std::vector<std::shared_ptr<BatchedCommandRequest>> kAllRequests = {
    createInsertRequest(kNss), createUpdateRequest(kNss), createDeleteRequest(kNss)};

TEST(UnifiedWriteExecutorProducerTest, BatchWriteOpProducerPeekAndAdvance) {

    for (const auto& requestPtr : kAllRequests) {
        LOGV2(10412800,
              "Running BatchWriteOpProducer test for request type",
              "requestType"_attr = getBatchTypeString(requestPtr->getBatchType()));

        MultiWriteOpProducer<BatchedCommandRequest> producer(*requestPtr);

        // Repeated peek and advance three times.
        auto op0 = producer.peekNext();
        ASSERT_TRUE(op0.has_value());
        ASSERT_EQ(op0->getId(), 0);
        producer.advance();

        auto op1 = producer.peekNext();
        ASSERT_TRUE(op1.has_value());
        ASSERT_EQ(op1->getId(), 1);
        producer.advance();

        auto op2 = producer.peekNext();
        ASSERT_TRUE(op2.has_value());
        ASSERT_EQ(op2->getId(), 2);
        producer.advance();

        auto noop = producer.peekNext();
        ASSERT_FALSE(noop.has_value());
    }
}

TEST(UnifiedWriteExecutorProducerTest, BatchWriteOpProducerRepeatedPeek) {

    for (const auto& requestPtr : kAllRequests) {
        LOGV2(10412801,
              "Running BatchWriteOpProducer test for request type",
              "requestType"_attr = getBatchTypeString(requestPtr->getBatchType()));

        MultiWriteOpProducer<BatchedCommandRequest> producer(*requestPtr);
        // Peek the same write op three times then advance.
        auto op0First = producer.peekNext();
        ASSERT_TRUE(op0First.has_value());
        ASSERT_EQ(op0First->getId(), 0);

        auto op0Second = producer.peekNext();
        ASSERT_TRUE(op0Second.has_value());
        ASSERT_EQ(op0Second->getId(), 0);

        auto op0Third = producer.peekNext();
        ASSERT_TRUE(op0Third.has_value());
        ASSERT_EQ(op0Third->getId(), 0);

        producer.advance();

        auto op1Second = producer.peekNext();
        ASSERT_TRUE(op1Second.has_value());
        ASSERT_EQ(op1Second->getId(), 1);

        producer.advance();

        auto op2Second = producer.peekNext();
        ASSERT_TRUE(op2Second.has_value());
        ASSERT_EQ(op2Second->getId(), 2);

        producer.advance();

        auto noop = producer.peekNext();
        ASSERT_FALSE(noop.has_value());
    }
}

TEST(UnifiedWriteExecutorProducerTest, BatchWriteOpProducerInsertContents) {
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("test", "coll0");
    auto insertRequest = createInsertRequest(nss);

    MultiWriteOpProducer<BatchedCommandRequest> producer(*insertRequest);

    auto op0 = producer.peekNext();
    ASSERT_TRUE(op0.has_value());
    ASSERT_EQ(op0->getId(), 0);
    ASSERT_EQ(op0->getNss(), nss);
    ASSERT_BSONOBJ_EQ(op0->getRef().getDocument(), BSON("x" << 0));
    producer.advance();

    auto op1 = producer.peekNext();
    ASSERT_TRUE(op1.has_value());
    ASSERT_EQ(op1->getId(), 1);
    ASSERT_EQ(op1->getNss(), nss);
    ASSERT_BSONOBJ_EQ(op1->getRef().getDocument(), BSON("x" << 1));
    producer.advance();

    auto op2 = producer.peekNext();
    ASSERT_TRUE(op2.has_value());
    ASSERT_EQ(op2->getId(), 2);
    ASSERT_EQ(op2->getNss(), nss);
    ASSERT_BSONOBJ_EQ(op2->getRef().getDocument(), BSON("x" << 2));
    producer.advance();

    auto noop = producer.peekNext();
    ASSERT_FALSE(noop.has_value());
}

TEST(UnifiedWriteExecutorProducerTest, BatchWriteOpProducerUpdateContents) {
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("test", "coll1");
    auto updateRequest = createUpdateRequest(nss);

    MultiWriteOpProducer<BatchedCommandRequest> producer(*updateRequest);

    auto op0 = producer.peekNext();
    ASSERT_TRUE(op0.has_value());
    ASSERT_EQ(op0->getId(), 0);
    ASSERT_EQ(op0->getNss(), nss);
    ASSERT_BSONOBJ_EQ(op0->getRef().getUpdateRef().getFilter(), BSON("_id" << 1));
    producer.advance();

    auto op1 = producer.peekNext();
    ASSERT_TRUE(op1.has_value());
    ASSERT_EQ(op1->getId(), 1);
    ASSERT_EQ(op1->getNss(), nss);
    ASSERT_BSONOBJ_EQ(op1->getRef().getUpdateRef().getFilter(), BSON("_id" << 2));
    producer.advance();

    auto op2 = producer.peekNext();
    ASSERT_TRUE(op2.has_value());
    ASSERT_EQ(op2->getId(), 2);
    ASSERT_EQ(op2->getNss(), nss);
    ASSERT_BSONOBJ_EQ(op2->getRef().getUpdateRef().getFilter(), BSON("_id" << 3));
    producer.advance();

    auto noop = producer.peekNext();
    ASSERT_FALSE(noop.has_value());
}

TEST(UnifiedWriteExecutorProducerTest, BatchWriteOpProducerDeleteContents) {

    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("test", "coll2");
    auto deleteRequest = createDeleteRequest(nss);

    MultiWriteOpProducer<BatchedCommandRequest> producer(*deleteRequest);

    auto op0 = producer.peekNext();
    ASSERT_TRUE(op0.has_value());
    ASSERT_EQ(op0->getId(), 0);
    ASSERT_EQ(op0->getNss(), nss);
    ASSERT_BSONOBJ_EQ(op0->getRef().getDeleteRef().getFilter(), BSON("x" << GTE << -1 << LT << 2));
    producer.advance();

    auto op1 = producer.peekNext();
    ASSERT_TRUE(op1.has_value());
    ASSERT_EQ(op1->getId(), 1);
    ASSERT_EQ(op1->getNss(), nss);
    ASSERT_BSONOBJ_EQ(op1->getRef().getDeleteRef().getFilter(), BSON("x" << GTE << -2 << LT << 1));
    producer.advance();

    auto op2 = producer.peekNext();
    ASSERT_TRUE(op2.has_value());
    ASSERT_EQ(op2->getId(), 2);
    ASSERT_EQ(op2->getNss(), nss);
    ASSERT_BSONOBJ_EQ(op2->getRef().getDeleteRef().getFilter(), BSON("x" << GTE << -3 << LT << 0));
    producer.advance();

    auto noop = producer.peekNext();
    ASSERT_FALSE(noop.has_value());
}

}  // namespace
}  // namespace unified_write_executor
}  // namespace mongo
