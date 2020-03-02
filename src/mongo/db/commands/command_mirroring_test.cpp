/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/logical_session_id.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

class CommandMirroringTest : public unittest::Test {
public:
    CommandMirroringTest() : _lsid(makeLogicalSessionIdForTest()) {}

    void setUp() override {
        setGlobalServiceContext(ServiceContext::make());
        Client::initThread("CommandMirroringTest"_sd);
    }

    void tearDown() override {
        auto client = Client::releaseCurrent();
        client.reset(nullptr);
    }
    virtual std::string commandName() = 0;

    virtual OpMsgRequest makeCommand(std::string coll, std::vector<BSONObj> args) {
        BSONObjBuilder bob;

        bob << commandName() << coll;
        bob << "lsid" << _lsid.toBSON();

        for (auto arg : args) {
            bob << arg.firstElement();
        }

        auto request = OpMsgRequest::fromDBAndBody(kDB, bob.obj());
        return request;
    }

    BSONObj createCommandAndGetMirrored(std::string coll, std::vector<BSONObj> args) {
        auto cmd = makeCommand(coll, args);
        return getMirroredCommand(cmd);
    }

    // Checks if "a" and "b" (both BSON objects) are equal.
    bool compareBSONObjs(BSONObj a, BSONObj b) {
        return (a == b).type == BSONObj::DeferredComparison::Type::kEQ;
    }

    static constexpr auto kDB = "test"_sd;

private:
    BSONObj getMirroredCommand(OpMsgRequest& request) {
        auto cmd = globalCommandRegistry()->findCommand(request.getCommandName());
        ASSERT(cmd);

        auto opCtx = cc().makeOperationContext();
        opCtx->setLogicalSessionId(_lsid);

        auto invocation = cmd->parse(opCtx.get(), request);
        ASSERT(invocation->supportsReadMirroring());

        BSONObjBuilder bob;
        invocation->appendMirrorableRequest(&bob);
        return bob.obj();
    }

    const LogicalSessionId _lsid;
};

class UpdateCommandTest : public CommandMirroringTest {
public:
    std::string commandName() override {
        return "update";
    }

    OpMsgRequest makeCommand(std::string coll, std::vector<BSONObj> updates) override {
        auto request = CommandMirroringTest::makeCommand(coll, {});

        // Directly add `updates` to `OpMsg::sequences` to emulate `OpMsg::parse()` behavior.
        OpMsg::DocumentSequence seq;
        seq.name = "updates";

        for (auto update : updates) {
            seq.objs.emplace_back(std::move(update));
        }
        request.sequences.emplace_back(std::move(seq));

        return request;
    }
};

TEST_F(UpdateCommandTest, NoQuery) {
    auto update = BSON("q" << BSONObj() << "u" << BSON("$set" << BSON("_id" << 1)));
    auto mirroredObj = createCommandAndGetMirrored("my_collection", {update});

    ASSERT_EQ(mirroredObj["find"].String(), "my_collection");
    ASSERT_EQ(mirroredObj["filter"].Obj().toString(), "{}");
    ASSERT(!mirroredObj.hasField("hint"));
    ASSERT(mirroredObj["singleBatch"].Bool());
    ASSERT_EQ(mirroredObj["batchSize"].Int(), 1);
}

TEST_F(UpdateCommandTest, SingleQuery) {
    auto update =
        BSON("q" << BSON("qty" << BSON("$lt" << 50.0)) << "u" << BSON("$inc" << BSON("qty" << 1)));
    auto mirroredObj = createCommandAndGetMirrored("products", {update});

    ASSERT_EQ(mirroredObj["find"].String(), "products");
    ASSERT_EQ(mirroredObj["filter"].Obj().toString(), "{ qty: { $lt: 50.0 } }");
    ASSERT(!mirroredObj.hasField("hint"));
    ASSERT(mirroredObj["singleBatch"].Bool());
    ASSERT_EQ(mirroredObj["batchSize"].Int(), 1);
}

TEST_F(UpdateCommandTest, SingleQueryWithHintAndCollation) {
    auto update = BSON("q" << BSON("price" << BSON("$gt" << 100)) << "hint" << BSON("price" << 1)
                           << "collation"
                           << BSON("locale"
                                   << "fr")
                           << "u" << BSON("$inc" << BSON("price" << 10)));

    auto mirroredObj = createCommandAndGetMirrored("products", {update});

    ASSERT_EQ(mirroredObj["find"].String(), "products");
    ASSERT_EQ(mirroredObj["filter"].Obj().toString(), "{ price: { $gt: 100 } }");
    ASSERT_EQ(mirroredObj["hint"].Obj().toString(), "{ price: 1 }");
    ASSERT_EQ(mirroredObj["collation"].Obj().toString(), "{ locale: \"fr\" }");
    ASSERT(mirroredObj["singleBatch"].Bool());
    ASSERT_EQ(mirroredObj["batchSize"].Int(), 1);
}

TEST_F(UpdateCommandTest, MultipleQueries) {
    constexpr int kUpdatesQ = 10;
    std::vector<BSONObj> updates;
    for (auto i = 0; i < kUpdatesQ; i++) {
        updates.emplace_back(BSON("q" << BSON("_id" << BSON("$eq" << i)) << "u"
                                      << BSON("$inc" << BSON("qty" << 1))));
    }
    auto mirroredObj = createCommandAndGetMirrored("products", updates);

    ASSERT_EQ(mirroredObj["find"].String(), "products");
    ASSERT_EQ(mirroredObj["filter"].Obj().toString(), "{ _id: { $eq: 0 } }");
    ASSERT(!mirroredObj.hasField("hint"));
    ASSERT(mirroredObj["singleBatch"].Bool());
    ASSERT_EQ(mirroredObj["batchSize"].Int(), 1);
}

class FindCommandTest : public CommandMirroringTest {
public:
    std::string commandName() override {
        return "find";
    }

    virtual std::vector<std::string> getAllowedKeys() const {
        return {"find",
                "filter",
                "skip",
                "limit",
                "sort",
                "hint",
                "collation",
                "min",
                "max",
                "batchSize",
                "singleBatch"};
    }

    void checkFieldNamesAreAllowed(BSONObj& mirroredObj) {
        const auto possibleKeys = getAllowedKeys();
        for (auto key : mirroredObj.getFieldNames<std::set<std::string>>()) {
            ASSERT(std::find(possibleKeys.begin(), possibleKeys.end(), key) != possibleKeys.end());
        }
    }
};

TEST_F(FindCommandTest, MirrorableKeys) {
    auto findArgs = {BSON("filter" << BSONObj()),
                     BSON("sort" << BSONObj()),
                     BSON("projection" << BSONObj()),
                     BSON("hint" << BSONObj()),
                     BSON("skip" << 1),
                     BSON("limit" << 1),
                     BSON("batchSize" << 1),
                     BSON("singleBatch" << true),
                     BSON("comment"
                          << "This is a comment."),
                     BSON("maxTimeMS" << 100),
                     BSON("readConcern"
                          << "primary"),
                     BSON("max" << BSONObj()),
                     BSON("min" << BSONObj()),
                     BSON("returnKey" << true),
                     BSON("showRecordId" << false),
                     BSON("tailable" << false),
                     BSON("oplogReplay" << true),
                     BSON("noCursorTimeout" << true),
                     BSON("awaitData" << true),
                     BSON("allowPartialResults" << true),
                     BSON("collation" << BSONObj())};

    auto mirroredObj = createCommandAndGetMirrored("test", findArgs);
    checkFieldNamesAreAllowed(mirroredObj);
}

TEST_F(FindCommandTest, BatchSizeReconfiguration) {
    auto findArgs = {
        BSON("filter" << BSONObj()), BSON("batchSize" << 100), BSON("singleBatch" << false)};

    auto mirroredObj = createCommandAndGetMirrored("test", findArgs);
    ASSERT_EQ(mirroredObj["singleBatch"].Bool(), true);
    ASSERT_EQ(mirroredObj["batchSize"].Int(), 1);
}

TEST_F(FindCommandTest, ValidateMirroredQuery) {
    constexpr auto collection = "restaurants";
    const auto filter = BSON("rating" << BSON("$gte" << 9) << "cuisine"
                                      << "Italian");
    constexpr auto skip = 10;
    constexpr auto limit = 50;
    const auto sortObj = BSON("name" << 1);
    const auto hint = BSONObj();
    const auto collation = BSON("locale"
                                << "\"fr\""
                                << "strength" << 1);
    const auto min = BSONObj();
    const auto max = BSONObj();

    auto findArgs = {BSON("filter" << filter),
                     BSON("skip" << skip),
                     BSON("limit" << limit),
                     BSON("sort" << sortObj),
                     BSON("hint" << hint),
                     BSON("collation" << collation),
                     BSON("min" << min),
                     BSON("max" << max)};

    auto mirroredObj = createCommandAndGetMirrored(collection, findArgs);

    ASSERT_EQ(mirroredObj["find"].String(), collection);
    ASSERT(compareBSONObjs(mirroredObj["filter"].Obj(), filter));
    ASSERT_EQ(mirroredObj["skip"].Int(), skip);
    ASSERT_EQ(mirroredObj["limit"].Int(), limit);
    ASSERT(compareBSONObjs(mirroredObj["sort"].Obj(), sortObj));
    ASSERT(compareBSONObjs(mirroredObj["hint"].Obj(), hint));
    ASSERT(compareBSONObjs(mirroredObj["collation"].Obj(), collation));
    ASSERT(compareBSONObjs(mirroredObj["min"].Obj(), min));
    ASSERT(compareBSONObjs(mirroredObj["max"].Obj(), max));
}

class FindAndModifyCommandTest : public FindCommandTest {
public:
    std::string commandName() override {
        return "findAndModify";
    }

    std::vector<std::string> getAllowedKeys() const override {
        return {"sort", "collation", "find", "filter", "batchSize", "singleBatch"};
    }
};

TEST_F(FindAndModifyCommandTest, MirrorableKeys) {
    auto findAndModifyArgs = {BSON("query" << BSONObj()),
                              BSON("sort" << BSONObj()),
                              BSON("remove" << false),
                              BSON("update" << BSONObj()),
                              BSON("new" << true),
                              BSON("fields" << BSONObj()),
                              BSON("upsert" << true),
                              BSON("bypassDocumentValidation" << false),
                              BSON("writeConcern" << BSONObj()),
                              BSON("maxTimeMS" << 100),
                              BSON("collation" << BSONObj()),
                              BSON("arrayFilters" << BSONArray())};

    auto mirroredObj = createCommandAndGetMirrored("test", findAndModifyArgs);
    checkFieldNamesAreAllowed(mirroredObj);
}

TEST_F(FindAndModifyCommandTest, BatchSizeReconfiguration) {
    auto findAndModifyArgs = {BSON("query" << BSONObj()),
                              BSON("update" << BSONObj()),
                              BSON("batchSize" << 100),
                              BSON("singleBatch" << false)};

    auto mirroredObj = createCommandAndGetMirrored("test", findAndModifyArgs);
    ASSERT_EQ(mirroredObj["singleBatch"].Bool(), true);
    ASSERT_EQ(mirroredObj["batchSize"].Int(), 1);
}

TEST_F(FindAndModifyCommandTest, ValidateMirroredQuery) {
    constexpr auto collection = "people";
    const auto query = BSON("name"
                            << "Andy");
    const auto sortObj = BSON("rating" << 1);
    const auto update = BSON("$inc" << BSON("score" << 1));
    constexpr auto upsert = true;
    const auto collation = BSON("locale"
                                << "\"fr\"");

    auto findAndModifyArgs = {BSON("query" << query),
                              BSON("sort" << sortObj),
                              BSON("update" << update),
                              BSON("upsert" << upsert),
                              BSON("collation" << collation)};

    auto mirroredObj = createCommandAndGetMirrored(collection, findAndModifyArgs);

    ASSERT_EQ(mirroredObj["find"].String(), collection);
    ASSERT(!mirroredObj.hasField("upsert"));
    ASSERT(compareBSONObjs(mirroredObj["filter"].Obj(), query));
    ASSERT(compareBSONObjs(mirroredObj["sort"].Obj(), sortObj));
    ASSERT(compareBSONObjs(mirroredObj["collation"].Obj(), collation));
}

class DistinctCommandTest : public FindCommandTest {
public:
    std::string commandName() override {
        return "distinct";
    }

    std::vector<std::string> getAllowedKeys() const override {
        return {"distinct", "key", "query", "collation"};
    }
};

TEST_F(DistinctCommandTest, MirrorableKeys) {
    auto distinctArgs = {BSON("key"
                              << ""),
                         BSON("query" << BSONObj()),
                         BSON("readConcern" << BSONObj()),
                         BSON("collation" << BSONObj())};

    auto mirroredObj = createCommandAndGetMirrored("test", distinctArgs);
    checkFieldNamesAreAllowed(mirroredObj);
}

TEST_F(DistinctCommandTest, ValidateMirroredQuery) {
    constexpr auto collection = "restaurants";
    constexpr auto key = "rating";
    const auto query = BSON("cuisine"
                            << "italian");
    const auto readConcern = BSON("level"
                                  << "majority");
    const auto collation = BSON("strength" << 1);

    auto distinctArgs = {BSON("key" << key),
                         BSON("query" << query),
                         BSON("readConcern" << readConcern),
                         BSON("collation" << collation)};

    auto mirroredObj = createCommandAndGetMirrored(collection, distinctArgs);

    ASSERT_EQ(mirroredObj["distinct"].String(), collection);
    ASSERT(!mirroredObj.hasField("readConcern"));
    ASSERT_EQ(mirroredObj["key"].String(), key);
    ASSERT(compareBSONObjs(mirroredObj["query"].Obj(), query));
    ASSERT(compareBSONObjs(mirroredObj["collation"].Obj(), collation));
}

class CountCommandTest : public FindCommandTest {
public:
    std::string commandName() override {
        return "count";
    }

    std::vector<std::string> getAllowedKeys() const override {
        return {"count", "query", "skip", "limit", "hint", "collation"};
    }
};

TEST_F(CountCommandTest, MirrorableKeys) {
    auto countArgs = {BSON("query" << BSONObj()),
                      BSON("limit" << 100),
                      BSON("skip" << 10),
                      BSON("hint" << BSONObj()),
                      BSON("readConcern" << BSONObj()),
                      BSON("collation" << BSONObj())};

    auto mirroredObj = createCommandAndGetMirrored("test", countArgs);
    checkFieldNamesAreAllowed(mirroredObj);
}

TEST_F(CountCommandTest, ValidateMirroredQuery) {
    constexpr auto collection = "orders";
    const auto query = BSON("status"
                            << "Delivered");
    const auto hint = BSON("status" << 1);
    constexpr auto limit = 1000;

    auto countArgs = {BSON("query" << query), BSON("hint" << hint), BSON("limit" << limit)};
    auto mirroredObj = createCommandAndGetMirrored(collection, countArgs);

    ASSERT_EQ(mirroredObj["count"].String(), collection);
    ASSERT(!mirroredObj.hasField("skip"));
    ASSERT(!mirroredObj.hasField("collation"));
    ASSERT(compareBSONObjs(mirroredObj["query"].Obj(), query));
    ASSERT(compareBSONObjs(mirroredObj["hint"].Obj(), hint));
    ASSERT_EQ(mirroredObj["limit"].Int(), limit);
}

}  // namespace
}  // namespace mongo
