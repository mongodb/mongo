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

#include "mongo/bson/json.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/unified_write_executor/write_op_producer.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace unified_write_executor {
namespace {

TEST(UnifiedWriteExecutorProducerTest, BulkWriteOpProducerPeekAndAdvance) {
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("test", "coll");
    BulkWriteCommandRequest request({BulkWriteInsertOp(0, BSONObj()),
                                     BulkWriteInsertOp(0, BSONObj()),
                                     BulkWriteInsertOp(0, BSONObj())},
                                    {NamespaceInfoEntry(nss)});
    BulkWriteOpProducer producer(request);

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
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("test", "coll");
    BulkWriteCommandRequest request(
        {BulkWriteInsertOp(0, BSONObj()), BulkWriteInsertOp(0, BSONObj())},
        {NamespaceInfoEntry(nss)});
    BulkWriteOpProducer producer(request);

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

TEST(UnifiedWriteExecutorProducerTest, BulkWriteOpProducerRepeatedAdvance) {
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("test", "coll");
    BulkWriteCommandRequest request({BulkWriteInsertOp(0, BSONObj()),
                                     BulkWriteInsertOp(0, BSONObj()),
                                     BulkWriteInsertOp(0, BSONObj())},
                                    {NamespaceInfoEntry(nss)});
    BulkWriteOpProducer producer(request);

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

TEST(UnifiedWriteExecutorProducerTest, BulkWriteOpProducerAdvancePastEnd) {
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("test", "coll");
    BulkWriteCommandRequest request({BulkWriteInsertOp(0, BSONObj())}, {NamespaceInfoEntry(nss)});
    BulkWriteOpProducer producer(request);

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

TEST(UnifiedWriteExecutorProducerTest, BulkWriteOpProducerReprocessSingle) {
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("test", "coll");
    BulkWriteCommandRequest request({BulkWriteInsertOp(0, BSONObj()),
                                     BulkWriteInsertOp(0, BSONObj()),
                                     BulkWriteInsertOp(0, BSONObj())},
                                    {NamespaceInfoEntry(nss)});
    BulkWriteOpProducer producer(request);

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

TEST(UnifiedWriteExecutorProducerTest, BulkWriteOpProducerReprocessLowThenHighId) {
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("test", "coll");
    BulkWriteCommandRequest request({BulkWriteInsertOp(0, BSONObj()),
                                     BulkWriteInsertOp(0, BSONObj()),
                                     BulkWriteInsertOp(0, BSONObj())},
                                    {NamespaceInfoEntry(nss)});
    BulkWriteOpProducer producer(request);

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

TEST(UnifiedWriteExecutorProducerTest, BulkWriteOpProducerReprocessHighThenLowId) {
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("test", "coll");
    BulkWriteCommandRequest request({BulkWriteInsertOp(0, BSONObj()),
                                     BulkWriteInsertOp(0, BSONObj()),
                                     BulkWriteInsertOp(0, BSONObj())},
                                    {NamespaceInfoEntry(nss)});
    BulkWriteOpProducer producer(request);

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

TEST(UnifiedWriteExecutorProducerTest, BulkWriteOpProducerReprocessSameOpAgain) {
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("test", "coll");
    BulkWriteCommandRequest request({BulkWriteInsertOp(0, BSONObj()),
                                     BulkWriteInsertOp(0, BSONObj()),
                                     BulkWriteInsertOp(0, BSONObj())},
                                    {NamespaceInfoEntry(nss)});
    BulkWriteOpProducer producer(request);

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
    BulkWriteOpProducer producer(request);

    // Check ops with different namespaces and request contents.
    auto op0 = producer.peekNext();
    ASSERT_TRUE(op0.has_value());
    ASSERT_EQ(op0->getId(), 0);
    ASSERT_EQ(op0->getBulkWriteOp().getNsInfo().getNs(), nss0);
    ASSERT_BSONOBJ_EQ(
        std::get<mongo::BulkWriteInsertOp>(op0->getBulkWriteOp().getOp()).getDocument(),
        BSON("a" << 0));
    producer.advance();

    auto op1 = producer.peekNext();
    ASSERT_TRUE(op1.has_value());
    ASSERT_EQ(op1->getId(), 1);
    ASSERT_EQ(op1->getBulkWriteOp().getNsInfo().getNs(), nss1);
    ASSERT_BSONOBJ_EQ(std::get<mongo::BulkWriteUpdateOp>(op1->getBulkWriteOp().getOp()).getFilter(),
                      BSON("a" << 1));
    producer.advance();

    auto op2 = producer.peekNext();
    ASSERT_TRUE(op2.has_value());
    ASSERT_EQ(op2->getId(), 2);
    ASSERT_EQ(op2->getBulkWriteOp().getNsInfo().getNs(), nss2);
    ASSERT_BSONOBJ_EQ(std::get<mongo::BulkWriteDeleteOp>(op2->getBulkWriteOp().getOp()).getFilter(),
                      BSON("a" << 2));
    producer.advance();

    auto noop = producer.peekNext();
    ASSERT_FALSE(noop.has_value());
}

}  // namespace
}  // namespace unified_write_executor
}  // namespace mongo
