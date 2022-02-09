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

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/db/ops/write_ops_parsers_test_helpers.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

TEST(CommandWriteOpsParsers, CommonFields_BypassDocumentValidation) {
    for (BSONElement bypassDocumentValidation : BSON_ARRAY(true << false << 1 << 0 << 1.0 << 0.0)) {
        auto cmd = BSON("insert"
                        << "bar"
                        << "documents" << BSON_ARRAY(BSONObj()) << "bypassDocumentValidation"
                        << bypassDocumentValidation);
        for (bool seq : {false, true}) {
            auto request = toOpMsg("foo", cmd, seq);
            auto op = InsertOp::parse(request);
            ASSERT_EQ(op.getWriteCommandRequestBase().getBypassDocumentValidation(),
                      shouldBypassDocumentValidationForCommand(cmd));
        }
    }
}

TEST(CommandWriteOpsParsers, CommonFields_Ordered) {
    for (bool ordered : {true, false}) {
        auto cmd = BSON("insert"
                        << "bar"
                        << "documents" << BSON_ARRAY(BSONObj()) << "ordered" << ordered);
        for (bool seq : {false, true}) {
            auto request = toOpMsg("foo", cmd, seq);
            auto op = InsertOp::parse(request);
            ASSERT_EQ(op.getWriteCommandRequestBase().getOrdered(), ordered);
        }
    }
}

TEST(CommandWriteOpsParsers, CommonFields_IgnoredFields) {
    // These flags are ignored, so there is nothing to check other than that this doesn't throw.
    auto cmd = BSON("insert"
                    << "bar"
                    << "documents" << BSON_ARRAY(BSONObj()) << "maxTimeMS" << 1000 << "shardVersion"
                    << BSONObj() << "writeConcern" << BSONObj());
    for (bool seq : {false, true}) {
        auto request = toOpMsg("foo", cmd, seq);
        InsertOp::parse(request);
    }
}

TEST(CommandWriteOpsParsers, GarbageFieldsAtTopLevel_Body) {
    auto cmd = BSON("insert"
                    << "bar"
                    << "documents" << BSON_ARRAY(BSONObj()) << "GARBAGE" << BSON_ARRAY(BSONObj()));
    for (bool seq : {false, true}) {
        auto request = toOpMsg("foo", cmd, seq);
        ASSERT_THROWS(InsertOp::parse(request), AssertionException);
    }
}

TEST(CommandWriteOpsParsers, ErrorOnDuplicateCommonField) {
    auto cmd =
        BSON("insert"
             << "bar"
             << "documents" << BSON_ARRAY(BSONObj()) << "documents" << BSON_ARRAY(BSONObj()));
    for (bool seq : {false, true}) {
        auto request = toOpMsg("foo", cmd, seq);
        ASSERT_THROWS(InsertOp::parse(request), AssertionException);
    }
}

TEST(CommandWriteOpsParsers, ErrorOnDuplicateCommonFieldBetweenBodyAndSequence) {
    OpMsgRequest request;
    request.body = BSON("insert"
                        << "bar"
                        << "documents" << BSON_ARRAY(BSONObj()) << "$db"
                        << "foo");
    request.sequences = {{"documents",
                          {
                              BSONObj(),
                          }}};

    ASSERT_THROWS(InsertOp::parse(request), AssertionException);
}

TEST(CommandWriteOpsParsers, ErrorOnWrongSizeStmtIdsArray) {
    auto cmd =
        BSON("insert"
             << "bar"
             << "documents" << BSON_ARRAY(BSONObj() << BSONObj()) << "stmtIds" << BSON_ARRAY(12));
    for (bool seq : {false, true}) {
        auto request = toOpMsg("foo", cmd, seq);
        ASSERT_THROWS_CODE(InsertOp::parse(request), AssertionException, ErrorCodes::InvalidLength);
    }
}

TEST(CommandWriteOpsParsers, ErrorOnStmtIdSpecifiedTwoWays) {
    auto cmd = BSON("insert"
                    << "bar"
                    << "documents" << BSON_ARRAY(BSONObj()) << "stmtIds" << BSON_ARRAY(12)
                    << "stmtId" << 13);
    for (bool seq : {false, true}) {
        auto request = toOpMsg("foo", cmd, seq);
        ASSERT_THROWS_CODE(
            InsertOp::parse(request), AssertionException, ErrorCodes::InvalidOptions);
    }
}

TEST(CommandWriteOpsParsers, GarbageFieldsInUpdateDoc) {
    auto cmd = BSON("update"
                    << "bar"
                    << "updates"
                    << BSON_ARRAY(BSON("q" << BSONObj() << "u" << BSONObj() << "GARBAGE" << 1)));
    for (bool seq : {false, true}) {
        auto request = toOpMsg("foo", cmd, seq);
        ASSERT_THROWS(UpdateOp::parse(request), AssertionException);
    }
}

TEST(CommandWriteOpsParsers, GarbageFieldsInDeleteDoc) {
    auto cmd =
        BSON("delete"
             << "bar"
             << "deletes" << BSON_ARRAY(BSON("q" << BSONObj() << "limit" << 0 << "GARBAGE" << 1)));
    for (bool seq : {false, true}) {
        auto request = toOpMsg("foo", cmd, seq);
        ASSERT_THROWS(DeleteOp::parse(request), AssertionException);
    }
}

TEST(CommandWriteOpsParsers, BadCollationFieldInUpdateDoc) {
    auto cmd = BSON("update"
                    << "bar"
                    << "updates"
                    << BSON_ARRAY(BSON("q" << BSONObj() << "u" << BSONObj() << "collation" << 1)));
    for (bool seq : {false, true}) {
        auto request = toOpMsg("foo", cmd, seq);
        ASSERT_THROWS_CODE(UpdateOp::parse(request), AssertionException, ErrorCodes::TypeMismatch);
    }
}

TEST(CommandWriteOpsParsers, BadCollationFieldInDeleteDoc) {
    auto cmd = BSON("delete"
                    << "bar"
                    << "deletes"
                    << BSON_ARRAY(BSON("q" << BSONObj() << "limit" << 0 << "collation" << 1)));
    for (bool seq : {false, true}) {
        auto request = toOpMsg("foo", cmd, seq);
        ASSERT_THROWS_CODE(DeleteOp::parse(request), AssertionException, ErrorCodes::TypeMismatch);
    }
}

TEST(CommandWriteOpsParsers, BadArrayFiltersFieldInUpdateDoc) {
    auto cmd = BSON("update"
                    << "bar"
                    << "updates"
                    << BSON_ARRAY(BSON("q" << BSONObj() << "u" << BSONObj() << "arrayFilters"
                                           << "bad")));
    for (bool seq : {false, true}) {
        auto request = toOpMsg("foo", cmd, seq);
        ASSERT_THROWS(UpdateOp::parse(request), AssertionException);
    }
}

TEST(CommandWriteOpsParsers, BadArrayFiltersElementInUpdateDoc) {
    auto cmd = BSON("update"
                    << "bar"
                    << "updates"
                    << BSON_ARRAY(BSON("q" << BSONObj() << "u" << BSONObj() << "arrayFilters"
                                           << BSON_ARRAY("bad"))));
    for (bool seq : {false, true}) {
        auto request = toOpMsg("foo", cmd, seq);
        ASSERT_THROWS_CODE(UpdateOp::parse(request), AssertionException, ErrorCodes::TypeMismatch);
    }
}

TEST(CommandWriteOpsParsers, SingleInsert) {
    const auto ns = NamespaceString("test", "foo");
    const BSONObj obj = BSON("x" << 1);
    auto cmd = BSON("insert" << ns.coll() << "documents" << BSON_ARRAY(obj));
    for (bool seq : {false, true}) {
        auto request = toOpMsg(ns.db(), cmd, seq);
        const auto op = InsertOp::parse(request);
        ASSERT_EQ(op.getNamespace().ns(), ns.ns());
        ASSERT(!op.getWriteCommandRequestBase().getBypassDocumentValidation());
        ASSERT(op.getWriteCommandRequestBase().getOrdered());
        ASSERT_EQ(op.getDocuments().size(), 1u);
        ASSERT_BSONOBJ_EQ(op.getDocuments()[0], obj);
    }
}

TEST(CommandWriteOpsParsers, EmptyMultiInsertFails) {
    const auto ns = NamespaceString("test", "foo");
    auto cmd = BSON("insert" << ns.coll() << "documents" << BSONArray());
    for (bool seq : {false, true}) {
        auto request = toOpMsg(ns.db(), cmd, seq);
        ASSERT_THROWS_CODE(InsertOp::parse(request), AssertionException, ErrorCodes::InvalidLength);
    }
}

TEST(CommandWriteOpsParsers, RealMultiInsert) {
    const auto ns = NamespaceString("test", "foo");
    const BSONObj obj0 = BSON("x" << 0);
    const BSONObj obj1 = BSON("x" << 1);
    auto cmd = BSON("insert" << ns.coll() << "documents" << BSON_ARRAY(obj0 << obj1));
    for (bool seq : {false, true}) {
        auto request = toOpMsg(ns.db(), cmd, seq);
        const auto op = InsertOp::parse(request);
        ASSERT_EQ(op.getNamespace().ns(), ns.ns());
        ASSERT(!op.getWriteCommandRequestBase().getBypassDocumentValidation());
        ASSERT(op.getWriteCommandRequestBase().getOrdered());
        ASSERT_EQ(op.getDocuments().size(), 2u);
        ASSERT_BSONOBJ_EQ(op.getDocuments()[0], obj0);
        ASSERT_BSONOBJ_EQ(op.getDocuments()[1], obj1);
        ASSERT_EQ(0, write_ops::getStmtIdForWriteAt(op, 0));
        ASSERT_EQ(1, write_ops::getStmtIdForWriteAt(op, 1));
    }
}

TEST(CommandWriteOpsParsers, MultiInsertWithStmtId) {
    const auto ns = NamespaceString("test", "foo");
    const BSONObj obj0 = BSON("x" << 0);
    const BSONObj obj1 = BSON("x" << 1);
    auto cmd =
        BSON("insert" << ns.coll() << "documents" << BSON_ARRAY(obj0 << obj1) << "stmtId" << 10);
    for (bool seq : {false, true}) {
        auto request = toOpMsg(ns.db(), cmd, seq);
        const auto op = InsertOp::parse(request);
        ASSERT_EQ(op.getNamespace().ns(), ns.ns());
        ASSERT(!op.getWriteCommandRequestBase().getBypassDocumentValidation());
        ASSERT(op.getWriteCommandRequestBase().getOrdered());
        ASSERT_EQ(op.getDocuments().size(), 2u);
        ASSERT_BSONOBJ_EQ(op.getDocuments()[0], obj0);
        ASSERT_BSONOBJ_EQ(op.getDocuments()[1], obj1);
        ASSERT_EQ(10, write_ops::getStmtIdForWriteAt(op, 0));
        ASSERT_EQ(11, write_ops::getStmtIdForWriteAt(op, 1));
    }
}

TEST(CommandWriteOpsParsers, MultiInsertWithStmtIdsArray) {
    const auto ns = NamespaceString("test", "foo");
    const BSONObj obj0 = BSON("x" << 0);
    const BSONObj obj1 = BSON("x" << 1);
    auto cmd = BSON("insert" << ns.coll() << "documents" << BSON_ARRAY(obj0 << obj1) << "stmtIds"
                             << BSON_ARRAY(15 << 17));
    for (bool seq : {false, true}) {
        auto request = toOpMsg(ns.db(), cmd, seq);
        const auto op = InsertOp::parse(request);
        ASSERT_EQ(op.getNamespace().ns(), ns.ns());
        ASSERT(!op.getWriteCommandRequestBase().getBypassDocumentValidation());
        ASSERT(op.getWriteCommandRequestBase().getOrdered());
        ASSERT_EQ(op.getDocuments().size(), 2u);
        ASSERT_BSONOBJ_EQ(op.getDocuments()[0], obj0);
        ASSERT_BSONOBJ_EQ(op.getDocuments()[1], obj1);
        ASSERT_EQ(15, write_ops::getStmtIdForWriteAt(op, 0));
        ASSERT_EQ(17, write_ops::getStmtIdForWriteAt(op, 1));
    }
}

TEST(CommandWriteOpsParsers, UpdateCommandRequest) {
    const auto ns = NamespaceString("test", "foo");
    const BSONObj query = BSON("x" << 1);
    const BSONObj update = BSON("$inc" << BSON("x" << 1));
    const BSONObj collation = BSON("locale"
                                   << "en_US");
    const BSONObj arrayFilter = BSON("i" << 0);
    for (bool upsert : {false, true}) {
        for (bool multi : {false, true}) {
            auto rawUpdate =
                BSON("q" << query << "u" << update << "arrayFilters" << BSON_ARRAY(arrayFilter)
                         << "multi" << multi << "upsert" << upsert << "collation" << collation);
            auto cmd = BSON("update" << ns.coll() << "updates" << BSON_ARRAY(rawUpdate));
            for (bool seq : {false, true}) {
                auto request = toOpMsg(ns.db(), cmd, seq);
                auto op = UpdateOp::parse(request);
                ASSERT_EQ(op.getNamespace().ns(), ns.ns());
                ASSERT(!op.getWriteCommandRequestBase().getBypassDocumentValidation());
                ASSERT_EQ(op.getWriteCommandRequestBase().getOrdered(), true);
                ASSERT_EQ(op.getUpdates().size(), 1u);
                ASSERT_BSONOBJ_EQ(op.getUpdates()[0].getQ(), query);

                const auto& updateMod = op.getUpdates()[0].getU();
                ASSERT(updateMod.type() == write_ops::UpdateModification::Type::kModifier);
                ASSERT_BSONOBJ_EQ(updateMod.getUpdateModifier(), update);

                ASSERT_BSONOBJ_EQ(write_ops::collationOf(op.getUpdates()[0]), collation);
                ASSERT_EQ(write_ops::arrayFiltersOf(op.getUpdates()[0]).size(), 1u);
                ASSERT_BSONOBJ_EQ(write_ops::arrayFiltersOf(op.getUpdates()[0]).front(),
                                  arrayFilter);
                ASSERT_EQ(op.getUpdates()[0].getUpsert(), upsert);
                ASSERT_EQ(op.getUpdates()[0].getMulti(), multi);
                ASSERT_BSONOBJ_EQ(op.getUpdates()[0].toBSON(), rawUpdate);
            }
        }
    }
}

TEST(CommandWriteOpsParsers, UpdateWithPipeline) {
    const auto ns = NamespaceString("test", "foo");
    const BSONObj query = BSON("q" << BSON("x" << 1));
    std::vector<BSONObj> pipeline{BSON("$addFields" << BSON("x" << 1))};
    const BSONObj update = BSON("u" << pipeline);
    const BSONObj collation = BSON("locale"
                                   << "en_US");
    for (bool upsert : {false, true}) {
        for (bool multi : {false, true}) {
            auto rawUpdate = BSON("q" << query["q"] << "u" << update["u"] << "multi" << multi
                                      << "upsert" << upsert << "collation" << collation);
            auto cmd = BSON("update" << ns.coll() << "updates" << BSON_ARRAY(rawUpdate));
            for (bool seq : {false, true}) {
                auto request = toOpMsg(ns.db(), cmd, seq);
                auto op = UpdateOp::parse(request);
                ASSERT_EQ(op.getNamespace().ns(), ns.ns());
                ASSERT(!op.getWriteCommandRequestBase().getBypassDocumentValidation());
                ASSERT_EQ(op.getWriteCommandRequestBase().getOrdered(), true);
                ASSERT_EQ(op.getUpdates().size(), 1u);
                ASSERT_BSONOBJ_EQ(op.getUpdates()[0].getQ(), query["q"].Obj());

                const auto& updateMod = op.getUpdates()[0].getU();
                const auto& updateModPipeline = updateMod.getUpdatePipeline();
                ASSERT(updateMod.type() == write_ops::UpdateModification::Type::kPipeline);
                ASSERT_EQ(updateModPipeline.size(), 1u);
                ASSERT_BSONOBJ_EQ(updateModPipeline[0], pipeline[0]);

                ASSERT_BSONOBJ_EQ(write_ops::collationOf(op.getUpdates()[0]), collation);
                ASSERT_EQ(op.getUpdates()[0].getUpsert(), upsert);
                ASSERT_EQ(op.getUpdates()[0].getMulti(), multi);
                ASSERT_BSONOBJ_EQ(op.getUpdates()[0].toBSON(), rawUpdate);
            }
        }
    }
}

TEST(CommandWriteOpsParsers, Remove) {
    const auto ns = NamespaceString("test", "foo");
    const BSONObj query = BSON("x" << 1);
    const BSONObj collation = BSON("locale"
                                   << "en_US");
    for (bool multi : {false, true}) {
        auto rawDelete =
            BSON("q" << query << "limit" << (multi ? 0 : 1) << "collation" << collation);
        auto cmd = BSON("delete" << ns.coll() << "deletes" << BSON_ARRAY(rawDelete));
        for (bool seq : {false, true}) {
            auto request = toOpMsg(ns.db(), cmd, seq);
            auto op = DeleteOp::parse(request);
            ASSERT_EQ(op.getNamespace().ns(), ns.ns());
            ASSERT(!op.getWriteCommandRequestBase().getBypassDocumentValidation());
            ASSERT_EQ(op.getWriteCommandRequestBase().getOrdered(), true);
            ASSERT_EQ(op.getDeletes().size(), 1u);
            ASSERT_BSONOBJ_EQ(op.getDeletes()[0].getQ(), query);
            ASSERT_BSONOBJ_EQ(write_ops::collationOf(op.getDeletes()[0]), collation);
            ASSERT_EQ(op.getDeletes()[0].getMulti(), multi);
            ASSERT_BSONOBJ_EQ(op.getDeletes()[0].toBSON(), rawDelete);
        }
    }
}

TEST(CommandWriteOpsParsers, RemoveErrorsWithBadLimit) {
    // Only 1 and 0 should be accepted.
    for (BSONElement limit : BSON_ARRAY(-1 << 2 << 0.5)) {
        auto cmd = BSON("delete"
                        << "bar"
                        << "deletes" << BSON_ARRAY(BSON("q" << BSONObj() << "limit" << limit)));
        for (bool seq : {false, true}) {
            auto request = toOpMsg("foo", cmd, seq);
            ASSERT_THROWS_CODE(
                DeleteOp::parse(request), AssertionException, ErrorCodes::FailedToParse);
        }
    }
}

TEST(LegacyWriteOpsParsers, SingleInsert) {
    const std::string ns = "test.foo";
    const BSONObj obj = BSON("x" << 1);
    for (bool continueOnError : {false, true}) {
        auto message = makeDeprecatedInsertMessage(
            ns, &obj, 1, continueOnError ? InsertOption_ContinueOnError : 0);
        const auto op = InsertOp::parseLegacy(message);
        ASSERT_EQ(op.getNamespace().ns(), ns);
        ASSERT(!op.getWriteCommandRequestBase().getBypassDocumentValidation());
        ASSERT_EQ(!op.getWriteCommandRequestBase().getOrdered(), continueOnError);
        ASSERT_EQ(op.getDocuments().size(), 1u);
        ASSERT_BSONOBJ_EQ(op.getDocuments()[0], obj);
    }
}

TEST(LegacyWriteOpsParsers, EmptyMultiInsertFails) {
    const std::string ns = "test.foo";
    for (bool continueOnError : {false, true}) {
        auto objs = std::vector<BSONObj>{};
        auto message = makeDeprecatedInsertMessage(
            ns, objs.data(), objs.size(), (continueOnError ? InsertOption_ContinueOnError : 0));
        ASSERT_THROWS_CODE(
            InsertOp::parseLegacy(message), AssertionException, ErrorCodes::InvalidLength);
    }
}

TEST(LegacyWriteOpsParsers, RealMultiInsert) {
    const std::string ns = "test.foo";
    const BSONObj obj0 = BSON("x" << 0);
    const BSONObj obj1 = BSON("x" << 1);
    for (bool continueOnError : {false, true}) {
        auto objs = std::vector<BSONObj>{obj0, obj1};
        auto message = makeDeprecatedInsertMessage(
            ns, objs.data(), objs.size(), continueOnError ? InsertOption_ContinueOnError : 0);
        const auto op = InsertOp::parseLegacy(message);
        ASSERT_EQ(op.getNamespace().ns(), ns);
        ASSERT(!op.getWriteCommandRequestBase().getBypassDocumentValidation());
        ASSERT_EQ(!op.getWriteCommandRequestBase().getOrdered(), continueOnError);
        ASSERT_EQ(op.getDocuments().size(), 2u);
        ASSERT_BSONOBJ_EQ(op.getDocuments()[0], obj0);
        ASSERT_BSONOBJ_EQ(op.getDocuments()[1], obj1);
    }
}

/**
 * Test OpTime serializer and deserializer when OpTime does not have term initailized.
 */
TEST(OpTimeSerdes, OpTimeWithoutTerm) {
    const auto fieldName = "opTime";
    repl::OpTime opTime(Timestamp(10, 20), repl::OpTime::kUninitializedTerm);
    BSONObjBuilder bob;

    write_ops::opTimeSerializerWithTermCheck(opTime, "opTime", &bob);

    auto bsonObj = bob.done();
    auto bsonElem = bsonObj[fieldName];

    ASSERT_FALSE(bsonElem.eoo());

    repl::OpTime retOpTime = write_ops::opTimeParser(bsonElem);

    ASSERT_EQ(opTime.getTimestamp(), retOpTime.getTimestamp());
    ASSERT_EQ(opTime.getTerm(), retOpTime.getTerm());
}

/**
 * Test OpTime serializer and deserializer when OpTime have term initailized.
 */
TEST(OpTimeSerdes, OpTimeWithTerm) {
    const auto fieldName = "opTime";
    repl::OpTime opTime(Timestamp(10, 20), 10);
    BSONObjBuilder bob;

    write_ops::opTimeSerializerWithTermCheck(opTime, "opTime", &bob);

    auto bsonObj = bob.done();
    auto bsonElem = bsonObj[fieldName];

    ASSERT_FALSE(bsonElem.eoo());

    repl::OpTime retOpTime = write_ops::opTimeParser(bsonElem);

    ASSERT_EQ(opTime.getTimestamp(), retOpTime.getTimestamp());
    ASSERT_EQ(opTime.getTerm(), retOpTime.getTerm());
}

/**
 * Test OpTime deserializer by directly passing Timestamp to the OpTime deserializer.
 */
TEST(OpTimeSerdes, DeserializeWithTimestamp) {
    const auto fieldName = "opTime";
    Timestamp timestamp(10, 20);
    BSONObjBuilder bob;

    bob.append(fieldName, timestamp);

    auto bsonObj = bob.done();
    auto bsonElem = bsonObj[fieldName];

    ASSERT_FALSE(bsonElem.eoo());

    repl::OpTime retOpTime = write_ops::opTimeParser(bsonElem);

    ASSERT_EQ(timestamp, retOpTime.getTimestamp());
    ASSERT_EQ(repl::OpTime::kUninitializedTerm, retOpTime.getTerm());
}

/**
 * Test OpTime deserializer by passing invalid BSON type.
 */
TEST(OpTimeSerdes, DeserializeWithInvalidBSONType) {
    const auto fieldName = "opTime";
    BSONObjBuilder bob;

    bob.append(fieldName, 100);

    auto bsonObj = bob.done();
    auto bsonElem = bsonObj[fieldName];

    ASSERT_FALSE(bsonElem.eoo());
    ASSERT_THROWS_CODE(write_ops::opTimeParser(bsonElem), DBException, ErrorCodes::TypeMismatch);
}

}  // namespace
}  // namespace mongo
