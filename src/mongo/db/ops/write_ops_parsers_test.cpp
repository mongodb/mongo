/**
 *    Copyright (C) 2016 MongoDB Inc.
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

#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/db/ops/write_ops_parsers.h"
#include "mongo/db/ops/write_ops_parsers_test_helpers.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

TEST(CommandWriteOpsParsers, CommonFields_BypassDocumentValidation) {
    for (BSONElement bypassDocumentValidation : BSON_ARRAY(true << false << 1 << 0 << 1.0 << 0.0)) {
        auto cmd = BSON("insert"
                        << "bar"
                        << "documents"
                        << BSON_ARRAY(BSONObj())
                        << "bypassDocumentValidation"
                        << bypassDocumentValidation);
        for (bool seq : {false, true}) {
            auto request = toOpMsg("foo", cmd, seq);
            auto op = InsertOp::parse(request);
            ASSERT_EQ(op.getWriteCommandBase().getBypassDocumentValidation(),
                      shouldBypassDocumentValidationForCommand(cmd));
        }
    }
}

TEST(CommandWriteOpsParsers, CommonFields_Ordered) {
    for (bool ordered : {true, false}) {
        auto cmd = BSON("insert"
                        << "bar"
                        << "documents"
                        << BSON_ARRAY(BSONObj())
                        << "ordered"
                        << ordered);
        for (bool seq : {false, true}) {
            auto request = toOpMsg("foo", cmd, seq);
            auto op = InsertOp::parse(request);
            ASSERT_EQ(op.getWriteCommandBase().getOrdered(), ordered);
        }
    }
}

TEST(CommandWriteOpsParsers, CommonFields_IgnoredFields) {
    // These flags are ignored, so there is nothing to check other than that this doesn't throw.
    auto cmd = BSON("insert"
                    << "bar"
                    << "documents"
                    << BSON_ARRAY(BSONObj())
                    << "maxTimeMS"
                    << 1000
                    << "shardVersion"
                    << BSONObj()
                    << "writeConcern"
                    << BSONObj());
    for (bool seq : {false, true}) {
        auto request = toOpMsg("foo", cmd, seq);
        InsertOp::parse(request);
    }
}

TEST(CommandWriteOpsParsers, GarbageFieldsAtTopLevel_Body) {
    auto cmd = BSON("insert"
                    << "bar"
                    << "documents"
                    << BSON_ARRAY(BSONObj())
                    << "GARBAGE"
                    << BSON_ARRAY(BSONObj()));
    for (bool seq : {false, true}) {
        auto request = toOpMsg("foo", cmd, seq);
        ASSERT_THROWS(InsertOp::parse(request), UserException);
    }
}

TEST(CommandWriteOpsParsers, ErrorOnDuplicateCommonField) {
    auto cmd = BSON("insert"
                    << "bar"
                    << "documents"
                    << BSON_ARRAY(BSONObj())
                    << "documents"
                    << BSON_ARRAY(BSONObj()));
    for (bool seq : {false, true}) {
        auto request = toOpMsg("foo", cmd, seq);
        ASSERT_THROWS(InsertOp::parse(request), UserException);
    }
}

TEST(CommandWriteOpsParsers, ErrorOnDuplicateCommonFieldBetweenBodyAndSequence) {
    OpMsgRequest request;
    request.body = BSON("insert"
                        << "bar"
                        << "documents"
                        << BSON_ARRAY(BSONObj())
                        << "$db"
                        << "foo");
    request.sequences = {{"documents",
                          {
                              BSONObj(),
                          }}};

    ASSERT_THROWS(InsertOp::parse(request), UserException);
}

TEST(CommandWriteOpsParsers, GarbageFieldsInUpdateDoc) {
    auto cmd = BSON("update"
                    << "bar"
                    << "updates"
                    << BSON_ARRAY(BSON("q" << BSONObj() << "u" << BSONObj() << "GARBAGE" << 1)));
    for (bool seq : {false, true}) {
        auto request = toOpMsg("foo", cmd, seq);
        ASSERT_THROWS(UpdateOp::parse(request), UserException);
    }
}

TEST(CommandWriteOpsParsers, GarbageFieldsInDeleteDoc) {
    auto cmd = BSON("delete"
                    << "bar"
                    << "deletes"
                    << BSON_ARRAY(BSON("q" << BSONObj() << "limit" << 0 << "GARBAGE" << 1)));
    for (bool seq : {false, true}) {
        auto request = toOpMsg("foo", cmd, seq);
        ASSERT_THROWS(DeleteOp::parse(request), UserException);
    }
}

TEST(CommandWriteOpsParsers, BadCollationFieldInUpdateDoc) {
    auto cmd = BSON("update"
                    << "bar"
                    << "updates"
                    << BSON_ARRAY(BSON("q" << BSONObj() << "u" << BSONObj() << "collation" << 1)));
    for (bool seq : {false, true}) {
        auto request = toOpMsg("foo", cmd, seq);
        ASSERT_THROWS_CODE(UpdateOp::parse(request), UserException, ErrorCodes::TypeMismatch);
    }
}

TEST(CommandWriteOpsParsers, BadCollationFieldInDeleteDoc) {
    auto cmd = BSON("delete"
                    << "bar"
                    << "deletes"
                    << BSON_ARRAY(BSON("q" << BSONObj() << "limit" << 0 << "collation" << 1)));
    for (bool seq : {false, true}) {
        auto request = toOpMsg("foo", cmd, seq);
        ASSERT_THROWS_CODE(DeleteOp::parse(request), UserException, ErrorCodes::TypeMismatch);
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
        ASSERT_THROWS(UpdateOp::parse(request), UserException);
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
        ASSERT_THROWS_CODE(UpdateOp::parse(request), UserException, ErrorCodes::TypeMismatch);
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
        ASSERT(!op.getWriteCommandBase().getBypassDocumentValidation());
        ASSERT(op.getWriteCommandBase().getOrdered());
        ASSERT_EQ(op.getDocuments().size(), 1u);
        ASSERT_BSONOBJ_EQ(op.getDocuments()[0], obj);
    }
}

TEST(CommandWriteOpsParsers, EmptyMultiInsertFails) {
    const auto ns = NamespaceString("test", "foo");
    auto cmd = BSON("insert" << ns.coll() << "documents" << BSONArray());
    for (bool seq : {false, true}) {
        auto request = toOpMsg(ns.db(), cmd, seq);
        ASSERT_THROWS_CODE(InsertOp::parse(request), UserException, ErrorCodes::InvalidLength);
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
        ASSERT(!op.getWriteCommandBase().getBypassDocumentValidation());
        ASSERT(op.getWriteCommandBase().getOrdered());
        ASSERT_EQ(op.getDocuments().size(), 2u);
        ASSERT_BSONOBJ_EQ(op.getDocuments()[0], obj0);
        ASSERT_BSONOBJ_EQ(op.getDocuments()[1], obj1);
    }
}

TEST(CommandWriteOpsParsers, Update) {
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
                         << "multi"
                         << multi
                         << "upsert"
                         << upsert
                         << "collation"
                         << collation);
            auto cmd = BSON("update" << ns.coll() << "updates" << BSON_ARRAY(rawUpdate));
            for (bool seq : {false, true}) {
                auto request = toOpMsg(ns.db(), cmd, seq);
                auto op = UpdateOp::parse(request);
                ASSERT_EQ(op.getNamespace().ns(), ns.ns());
                ASSERT(!op.getWriteCommandBase().getBypassDocumentValidation());
                ASSERT_EQ(op.getWriteCommandBase().getOrdered(), true);
                ASSERT_EQ(op.getUpdates().size(), 1u);
                ASSERT_BSONOBJ_EQ(op.getUpdates()[0].getQ(), query);
                ASSERT_BSONOBJ_EQ(op.getUpdates()[0].getU(), update);
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
            ASSERT(!op.getWriteCommandBase().getBypassDocumentValidation());
            ASSERT_EQ(op.getWriteCommandBase().getOrdered(), true);
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
                        << "deletes"
                        << BSON_ARRAY(BSON("q" << BSONObj() << "limit" << limit)));
        for (bool seq : {false, true}) {
            auto request = toOpMsg("foo", cmd, seq);
            ASSERT_THROWS_CODE(DeleteOp::parse(request), UserException, ErrorCodes::FailedToParse);
        }
    }
}

TEST(LegacyWriteOpsParsers, SingleInsert) {
    const std::string ns = "test.foo";
    const BSONObj obj = BSON("x" << 1);
    for (bool continueOnError : {false, true}) {
        auto message =
            makeInsertMessage(ns, obj, continueOnError ? InsertOption_ContinueOnError : 0);
        const auto op = InsertOp::parseLegacy(message);
        ASSERT_EQ(op.getNamespace().ns(), ns);
        ASSERT(!op.getWriteCommandBase().getBypassDocumentValidation());
        ASSERT_EQ(!op.getWriteCommandBase().getOrdered(), continueOnError);
        ASSERT_EQ(op.getDocuments().size(), 1u);
        ASSERT_BSONOBJ_EQ(op.getDocuments()[0], obj);
    }
}

TEST(LegacyWriteOpsParsers, EmptyMultiInsertFails) {
    const std::string ns = "test.foo";
    for (bool continueOnError : {false, true}) {
        auto objs = std::vector<BSONObj>{};
        auto message = makeInsertMessage(
            ns, objs.data(), objs.size(), (continueOnError ? InsertOption_ContinueOnError : 0));
        ASSERT_THROWS_CODE(
            InsertOp::parseLegacy(message), UserException, ErrorCodes::InvalidLength);
    }
}

TEST(LegacyWriteOpsParsers, RealMultiInsert) {
    const std::string ns = "test.foo";
    const BSONObj obj0 = BSON("x" << 0);
    const BSONObj obj1 = BSON("x" << 1);
    for (bool continueOnError : {false, true}) {
        auto objs = std::vector<BSONObj>{obj0, obj1};
        auto message = makeInsertMessage(
            ns, objs.data(), objs.size(), continueOnError ? InsertOption_ContinueOnError : 0);
        const auto op = InsertOp::parseLegacy(message);
        ASSERT_EQ(op.getNamespace().ns(), ns);
        ASSERT(!op.getWriteCommandBase().getBypassDocumentValidation());
        ASSERT_EQ(!op.getWriteCommandBase().getOrdered(), continueOnError);
        ASSERT_EQ(op.getDocuments().size(), 2u);
        ASSERT_BSONOBJ_EQ(op.getDocuments()[0], obj0);
        ASSERT_BSONOBJ_EQ(op.getDocuments()[1], obj1);
    }
}

TEST(LegacyWriteOpsParsers, Update) {
    const std::string ns = "test.foo";
    const BSONObj query = BSON("x" << 1);
    const BSONObj update = BSON("$inc" << BSON("x" << 1));
    for (bool upsert : {false, true}) {
        for (bool multi : {false, true}) {
            auto message = makeUpdateMessage(ns,
                                             query,
                                             update,
                                             (upsert ? UpdateOption_Upsert : 0) |
                                                 (multi ? UpdateOption_Multi : 0));
            const auto op = UpdateOp::parseLegacy(message);
            ASSERT_EQ(op.getNamespace().ns(), ns);
            ASSERT(!op.getWriteCommandBase().getBypassDocumentValidation());
            ASSERT_EQ(op.getWriteCommandBase().getOrdered(), true);
            ASSERT_EQ(op.getUpdates().size(), 1u);
            ASSERT_BSONOBJ_EQ(op.getUpdates()[0].getQ(), query);
            ASSERT_BSONOBJ_EQ(op.getUpdates()[0].getU(), update);
            ASSERT_EQ(op.getUpdates()[0].getUpsert(), upsert);
            ASSERT_EQ(op.getUpdates()[0].getMulti(), multi);
        }
    }
}

TEST(LegacyWriteOpsParsers, Remove) {
    const std::string ns = "test.foo";
    const BSONObj query = BSON("x" << 1);
    for (bool multi : {false, true}) {
        auto message = makeRemoveMessage(ns, query, (multi ? 0 : RemoveOption_JustOne));
        const auto op = DeleteOp::parseLegacy(message);
        ASSERT_EQ(op.getNamespace().ns(), ns);
        ASSERT(!op.getWriteCommandBase().getBypassDocumentValidation());
        ASSERT_EQ(op.getWriteCommandBase().getOrdered(), true);
        ASSERT_EQ(op.getDeletes().size(), 1u);
        ASSERT_BSONOBJ_EQ(op.getDeletes()[0].getQ(), query);
        ASSERT_EQ(op.getDeletes()[0].getMulti(), multi);
    }
}

}  // namespace
}  // namespace mongo
