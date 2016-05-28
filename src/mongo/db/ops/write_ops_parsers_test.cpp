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

#include "mongo/client/dbclientinterface.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/ops/write_ops_parsers.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

TEST(CommandWriteOpsParsers, CommonFields_BypassDocumentValidation) {
    for (BSONElement bypassDocumentValidation : BSON_ARRAY(true << false << 1 << 0 << 1.0 << 0.0)) {
        auto cmd = BSON("insert"
                        << "bar"
                        << "documents"
                        << BSON_ARRAY(BSONObj())
                        << "bypassDocumentValidation"
                        << bypassDocumentValidation);
        auto op = parseInsertCommand("foo", cmd);
        ASSERT_EQ(op.bypassDocumentValidation, shouldBypassDocumentValidationForCommand(cmd));
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
        auto op = parseInsertCommand("foo", cmd);
        ASSERT_EQ(op.continueOnError, !ordered);
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
    parseInsertCommand("foo", cmd);
}

TEST(CommandWriteOpsParsers, GarbageFieldsAtTopLevel) {
    auto cmd = BSON("insert"
                    << "bar"
                    << "documents"
                    << BSON_ARRAY(BSONObj())
                    << "GARBAGE"
                    << 1);
    ASSERT_THROWS_CODE(parseInsertCommand("foo", cmd), UserException, ErrorCodes::FailedToParse);
}

TEST(CommandWriteOpsParsers, GarbageFieldsInUpdateDoc) {
    auto cmd = BSON("update"
                    << "bar"
                    << "updates"
                    << BSON_ARRAY("q" << BSONObj() << "u" << BSONObj() << "GARBAGE" << 1));
    ASSERT_THROWS_CODE(parseInsertCommand("foo", cmd), UserException, ErrorCodes::FailedToParse);
}

TEST(CommandWriteOpsParsers, GarbageFieldsInDeleteDoc) {
    auto cmd = BSON("delete"
                    << "bar"
                    << "deletes"
                    << BSON_ARRAY("q" << BSONObj() << "limit" << 0 << "GARBAGE" << 1));
}

TEST(CommandWriteOpsParsers, BadCollationFieldInUpdateDoc) {
    auto cmd = BSON("update"
                    << "bar"
                    << "updates"
                    << BSON_ARRAY("q" << BSONObj() << "u" << BSONObj() << "collation" << 1));
    ASSERT_THROWS_CODE(parseInsertCommand("foo", cmd), UserException, ErrorCodes::FailedToParse);
}

TEST(CommandWriteOpsParsers, BadCollationFieldInDeleteDoc) {
    auto cmd = BSON("delete"
                    << "bar"
                    << "deletes"
                    << BSON_ARRAY("q" << BSONObj() << "limit" << 0 << "collation" << 1));
    ASSERT_THROWS_CODE(parseInsertCommand("foo", cmd), UserException, ErrorCodes::FailedToParse);
}

TEST(CommandWriteOpsParsers, SingleInsert) {
    const auto ns = NamespaceString("test", "foo");
    const BSONObj obj = BSON("x" << 1);
    auto cmd = BSON("insert" << ns.coll() << "documents" << BSON_ARRAY(obj));
    const auto op = parseInsertCommand(ns.db(), cmd);
    ASSERT_EQ(op.ns.ns(), ns.ns());
    ASSERT(!op.bypassDocumentValidation);
    ASSERT(!op.continueOnError);
    ASSERT_EQ(op.documents.size(), 1u);
    ASSERT_EQ(op.documents[0], obj);
}

TEST(CommandWriteOpsParsers, EmptyMultiInsertFails) {
    const auto ns = NamespaceString("test", "foo");
    auto cmd = BSON("insert" << ns.coll() << "documents" << BSONArray());
    ASSERT_THROWS_CODE(parseInsertCommand(ns.db(), cmd), UserException, ErrorCodes::InvalidLength);
}

TEST(CommandWriteOpsParsers, RealMultiInsert) {
    const auto ns = NamespaceString("test", "foo");
    const BSONObj obj0 = BSON("x" << 0);
    const BSONObj obj1 = BSON("x" << 1);
    auto cmd = BSON("insert" << ns.coll() << "documents" << BSON_ARRAY(obj0 << obj1));
    const auto op = parseInsertCommand(ns.db(), cmd);
    ASSERT_EQ(op.ns.ns(), ns.ns());
    ASSERT(!op.bypassDocumentValidation);
    ASSERT(!op.continueOnError);
    ASSERT_EQ(op.documents.size(), 2u);
    ASSERT_EQ(op.documents[0], obj0);
    ASSERT_EQ(op.documents[1], obj1);
}

TEST(CommandWriteOpsParsers, Update) {
    const auto ns = NamespaceString("test", "foo");
    const BSONObj query = BSON("x" << 1);
    const BSONObj update = BSON("$inc" << BSON("x" << 1));
    const BSONObj collation = BSON("locale"
                                   << "en_US");
    for (bool upsert : {false, true}) {
        for (bool multi : {false, true}) {
            auto cmd = BSON("update" << ns.coll() << "updates"
                                     << BSON_ARRAY(BSON("q" << query << "u" << update << "collation"
                                                            << collation
                                                            << "upsert"
                                                            << upsert
                                                            << "multi"
                                                            << multi)));
            auto op = parseUpdateCommand(ns.db(), cmd);
            ASSERT_EQ(op.ns.ns(), ns.ns());
            ASSERT(!op.bypassDocumentValidation);
            ASSERT_EQ(op.continueOnError, false);
            ASSERT_EQ(op.updates.size(), 1u);
            ASSERT_EQ(op.updates[0].query, query);
            ASSERT_EQ(op.updates[0].update, update);
            ASSERT_EQ(op.updates[0].collation, collation);
            ASSERT_EQ(op.updates[0].upsert, upsert);
            ASSERT_EQ(op.updates[0].multi, multi);
        }
    }
}

TEST(CommandWriteOpsParsers, Remove) {
    const auto ns = NamespaceString("test", "foo");
    const BSONObj query = BSON("x" << 1);
    const BSONObj collation = BSON("locale"
                                   << "en_US");
    for (bool multi : {false, true}) {
        auto cmd =
            BSON("delete" << ns.coll() << "deletes"
                          << BSON_ARRAY(BSON("q" << query << "collation" << collation << "limit"
                                                 << (multi ? 0 : 1))));
        auto op = parseDeleteCommand(ns.db(), cmd);
        ASSERT_EQ(op.ns.ns(), ns.ns());
        ASSERT(!op.bypassDocumentValidation);
        ASSERT_EQ(op.continueOnError, false);
        ASSERT_EQ(op.deletes.size(), 1u);
        ASSERT_EQ(op.deletes[0].query, query);
        ASSERT_EQ(op.deletes[0].collation, collation);
        ASSERT_EQ(op.deletes[0].multi, multi);
    }
}

TEST(CommandWriteOpsParsers, RemoveErrorsWithBadLimit) {
    // Only 1 and 0 should be accepted.
    for (BSONElement limit : BSON_ARRAY(-1 << 2 << 0.5)) {
        auto cmd = BSON("delete"
                        << "bar"
                        << "deletes"
                        << BSON_ARRAY("q" << BSONObj() << "limit" << limit));
        ASSERT_THROWS_CODE(
            parseInsertCommand("foo", cmd), UserException, ErrorCodes::FailedToParse);
    }
}

namespace {
/**
 * A mock DBClient that just captures the Message that is sent for legacy writes.
 */
class MyMockDBClient final : public DBClientBase {
public:
    Message message;  // The last message sent.

    void say(Message& toSend, bool isRetry = false, std::string* actualServer = nullptr) {
        message = std::move(toSend);
    }

    // The rest of these are just filling out the pure-virtual parts of the interface.
    bool lazySupported() const {
        return false;
    }
    std::string getServerAddress() const {
        return "";
    }
    std::string toString() const {
        return "";
    }
    bool call(Message& toSend, Message& response, bool assertOk, std::string* actualServer) {
        invariant(!"call() not implemented");
    }
    virtual int getMinWireVersion() {
        return 0;
    }
    virtual int getMaxWireVersion() {
        return 0;
    }
    virtual bool isFailed() const {
        return false;
    }
    virtual bool isStillConnected() {
        return true;
    }
    virtual double getSoTimeout() const {
        return 0;
    }
    virtual ConnectionString::ConnectionType type() const {
        return ConnectionString::MASTER;
    }
};
}  // namespace

TEST(LegacyWriteOpsParsers, SingleInsert) {
    const std::string ns = "test.foo";
    const BSONObj obj = BSON("x" << 1);
    for (bool continueOnError : {false, true}) {
        MyMockDBClient client;
        client.insert(ns, obj, continueOnError ? InsertOption_ContinueOnError : 0);
        const auto op = parseLegacyInsert(client.message);
        ASSERT_EQ(op.ns.ns(), ns);
        ASSERT(!op.bypassDocumentValidation);
        ASSERT_EQ(op.continueOnError, continueOnError);
        ASSERT_EQ(op.documents.size(), 1u);
        ASSERT_EQ(op.documents[0], obj);
    }
}

TEST(LegacyWriteOpsParsers, EmptyMultiInsertFails) {
    const std::string ns = "test.foo";
    for (bool continueOnError : {false, true}) {
        MyMockDBClient client;
        client.insert(
            ns, std::vector<BSONObj>{}, continueOnError ? InsertOption_ContinueOnError : 0);
        ASSERT_THROWS_CODE(
            parseLegacyInsert(client.message), UserException, ErrorCodes::InvalidLength);
    }
}

TEST(LegacyWriteOpsParsers, RealMultiInsert) {
    const std::string ns = "test.foo";
    const BSONObj obj0 = BSON("x" << 0);
    const BSONObj obj1 = BSON("x" << 1);
    for (bool continueOnError : {false, true}) {
        MyMockDBClient client;
        client.insert(ns, {obj0, obj1}, continueOnError ? InsertOption_ContinueOnError : 0);
        const auto op = parseLegacyInsert(client.message);
        ASSERT_EQ(op.ns.ns(), ns);
        ASSERT(!op.bypassDocumentValidation);
        ASSERT_EQ(op.continueOnError, continueOnError);
        ASSERT_EQ(op.documents.size(), 2u);
        ASSERT_EQ(op.documents[0], obj0);
        ASSERT_EQ(op.documents[1], obj1);
    }
}

TEST(LegacyWriteOpsParsers, Update) {
    const std::string ns = "test.foo";
    const BSONObj query = BSON("x" << 1);
    const BSONObj update = BSON("$inc" << BSON("x" << 1));
    for (bool upsert : {false, true}) {
        for (bool multi : {false, true}) {
            MyMockDBClient client;
            client.update(ns, query, update, upsert, multi);
            const auto op = parseLegacyUpdate(client.message);
            ASSERT_EQ(op.ns.ns(), ns);
            ASSERT(!op.bypassDocumentValidation);
            ASSERT_EQ(op.continueOnError, false);
            ASSERT_EQ(op.updates.size(), 1u);
            ASSERT_EQ(op.updates[0].query, query);
            ASSERT_EQ(op.updates[0].update, update);
            ASSERT_EQ(op.updates[0].upsert, upsert);
            ASSERT_EQ(op.updates[0].multi, multi);
        }
    }
}

TEST(LegacyWriteOpsParsers, Remove) {
    const std::string ns = "test.foo";
    const BSONObj query = BSON("x" << 1);
    for (bool multi : {false, true}) {
        MyMockDBClient client;
        client.remove(ns, query, multi ? 0 : RemoveOption_JustOne);
        const auto op = parseLegacyDelete(client.message);
        ASSERT_EQ(op.ns.ns(), ns);
        ASSERT(!op.bypassDocumentValidation);
        ASSERT_EQ(op.continueOnError, false);
        ASSERT_EQ(op.deletes.size(), 1u);
        ASSERT_EQ(op.deletes[0].query, query);
        ASSERT_EQ(op.deletes[0].multi, multi);
    }
}
}
