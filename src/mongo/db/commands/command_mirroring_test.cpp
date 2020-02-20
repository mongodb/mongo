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

    virtual BSONObj makeCommand(std::string, std::vector<BSONObj>) = 0;

    const LogicalSessionId& getLogicalSessionId() const {
        return _lsid;
    }

    BSONObj getMirroredCommand(BSONObj& bson) {
        auto request = OpMsgRequest::fromDBAndBody(kDB, bson);
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

    static constexpr auto kDB = "test"_sd;

private:
    const LogicalSessionId _lsid;
};

class UpdateCommandTest : public CommandMirroringTest {
public:
    BSONObj makeCommand(std::string coll, std::vector<BSONObj> updates) override {
        BSONObjBuilder bob;

        bob << "update" << coll;
        BSONArrayBuilder bab;
        for (auto update : updates) {
            bab << update;
        }
        bob << "updates" << bab.arr();
        bob << "lsid" << getLogicalSessionId().toBSON();

        return bob.obj();
    }
};

TEST_F(UpdateCommandTest, NoQuery) {
    auto update = BSON("q" << BSONObj() << "u" << BSON("$set" << BSON("_id" << 1)));
    auto cmd = makeCommand("my_collection", {update});

    auto mirroredObj = getMirroredCommand(cmd);

    ASSERT_EQ(mirroredObj["find"].String(), "my_collection");
    ASSERT_EQ(mirroredObj["filter"].Obj().toString(), "{}");
    ASSERT(!mirroredObj.hasField("hint"));
    ASSERT(mirroredObj["singleBatch"].Bool());
    ASSERT_EQ(mirroredObj["batchSize"].Int(), 1);
}

TEST_F(UpdateCommandTest, SingleQuery) {
    auto update =
        BSON("q" << BSON("qty" << BSON("$lt" << 50.0)) << "u" << BSON("$inc" << BSON("qty" << 1)));
    auto cmd = makeCommand("products", {update});

    auto mirroredObj = getMirroredCommand(cmd);

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
    auto cmd = makeCommand("products", {update});

    auto mirroredObj = getMirroredCommand(cmd);

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
    auto cmd = makeCommand("products", updates);

    auto mirroredObj = getMirroredCommand(cmd);

    ASSERT_EQ(mirroredObj["find"].String(), "products");
    ASSERT_EQ(mirroredObj["filter"].Obj().toString(), "{ _id: { $eq: 0 } }");
    ASSERT(!mirroredObj.hasField("hint"));
    ASSERT(mirroredObj["singleBatch"].Bool());
    ASSERT_EQ(mirroredObj["batchSize"].Int(), 1);
}

}  // namespace
}  // namespace mongo
