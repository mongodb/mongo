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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/db/versioning_protocol/shard_version_gen.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/unittest/unittest.h"

#include <algorithm>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace {

class CommandMirroringTest : public unittest::Test {
public:
    CommandMirroringTest() : _lsid(makeLogicalSessionIdForTest()) {}

    void setUp() override {
        setGlobalServiceContext(ServiceContext::make());
        Client::initThread("CommandMirroringTest"_sd, getGlobalServiceContext()->getService());
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

        for (const auto& arg : args) {
            bob << arg.firstElement();
        }

        auto request =
            OpMsgRequestBuilder::create(auth::ValidatedTenancyScope::kNotRequired,
                                        DatabaseName::createDatabaseName_forTest(boost::none, kDB),
                                        bob.obj());
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

    static constexpr auto kDB = "testDB"_sd;
    const ShardVersionBase kShardVersion = []() {
        ShardVersionBase sv;
        sv.setEpoch(OID());
        sv.setTimestamp(Timestamp(0, 1));
        sv.setVersion(Timestamp(0, 1));
        return sv;
    }();

    const DatabaseVersionBase kDatabaseVersion = []() {
        DatabaseVersionBase dv;
        dv.setLastMod(123);
        dv.setTimestamp(Timestamp(0, 1));
        return dv;
    }();

private:
    BSONObj getMirroredCommand(OpMsgRequest& request) {
        auto opCtx = cc().makeOperationContext();
        opCtx->setLogicalSessionId(_lsid);

        auto cmd = getCommandRegistry(opCtx.get())->findCommand(request.getCommandName());
        ASSERT(cmd);

        auto invocation = cmd->parse(opCtx.get(), request);
        if (!invocation->supportsReadMirroring()) {
            uasserted(ErrorCodes::CommandNotSupported, "command does not support read mirroring");
        }
        ASSERT_EQ(invocation->getDBForReadMirroring(),
                  DatabaseName::createDatabaseName_forTest(boost::none, kDB));

        BSONObjBuilder bob;
        invocation->appendMirrorableRequest(&bob);

        return bob.obj();
    }

    const LogicalSessionId _lsid;

protected:
    const std::string kCollection = "testColl";
    const std::string kNss = kDB + "." + kCollection;
};

class UpdateCommandTest : public CommandMirroringTest {
public:
    void setUp() override {
        CommandMirroringTest::setUp();
        shardVersion = boost::none;
        databaseVersion = boost::none;
    }

    std::string commandName() override {
        return "update";
    }

    OpMsgRequest makeCommand(std::string coll, std::vector<BSONObj> updates) override {
        std::vector<BSONObj> args;
        if (shardVersion) {
            args.push_back(shardVersion.value());
        }
        if (databaseVersion) {
            args.push_back(databaseVersion.value());
        }
        if (encryptionInformation) {
            args.push_back(encryptionInformation.value());
        }
        if (rawData) {
            args.push_back(rawData.value());
        }

        auto request = CommandMirroringTest::makeCommand(coll, args);

        // Directly add `updates` to `OpMsg::sequences` to emulate `OpMsg::parse()` behavior.
        OpMsg::DocumentSequence seq;
        seq.name = "updates";

        for (auto update : updates) {
            seq.objs.emplace_back(std::move(update));
        }
        request.sequences.emplace_back(std::move(seq));

        return request;
    }

    boost::optional<BSONObj> shardVersion;
    boost::optional<BSONObj> databaseVersion;
    boost::optional<BSONObj> encryptionInformation;
    boost::optional<BSONObj> rawData;
};

TEST_F(UpdateCommandTest, NoQuery) {
    auto update = BSON("q" << BSONObj() << "u" << BSON("$set" << BSON("_id" << 1)));
    auto mirroredObj = createCommandAndGetMirrored(kCollection, {update});

    ASSERT_EQ(mirroredObj["find"].String(), kCollection);
    ASSERT_EQ(mirroredObj["filter"].Obj().toString(), "{}");
    ASSERT(!mirroredObj.hasField("hint"));
    ASSERT(mirroredObj["singleBatch"].Bool());
    ASSERT_EQ(mirroredObj["batchSize"].Int(), 1);
}

TEST_F(UpdateCommandTest, SingleQuery) {
    auto update =
        BSON("q" << BSON("qty" << BSON("$lt" << 50.0)) << "u" << BSON("$inc" << BSON("qty" << 1)));
    auto mirroredObj = createCommandAndGetMirrored(kCollection, {update});

    ASSERT_EQ(mirroredObj["find"].String(), kCollection);
    ASSERT_EQ(mirroredObj["filter"].Obj().toString(), "{ qty: { $lt: 50.0 } }");
    ASSERT(!mirroredObj.hasField("hint"));
    ASSERT(mirroredObj["singleBatch"].Bool());
    ASSERT_EQ(mirroredObj["batchSize"].Int(), 1);
}

TEST_F(UpdateCommandTest, SingleQueryWithHintAndCollation) {
    auto update = BSON("q" << BSON("price" << BSON("$gt" << 100)) << "hint" << BSON("price" << 1)
                           << "collation" << BSON("locale" << "fr") << "u"
                           << BSON("$inc" << BSON("price" << 10)));

    auto mirroredObj = createCommandAndGetMirrored(kCollection, {update});

    ASSERT_EQ(mirroredObj["find"].String(), kCollection);
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
    auto mirroredObj = createCommandAndGetMirrored(kCollection, updates);

    ASSERT_EQ(mirroredObj["find"].String(), kCollection);
    ASSERT_EQ(mirroredObj["filter"].Obj().toString(), "{ _id: { $eq: 0 } }");
    ASSERT(!mirroredObj.hasField("hint"));
    ASSERT(mirroredObj["singleBatch"].Bool());
    ASSERT_EQ(mirroredObj["batchSize"].Int(), 1);
}

TEST_F(UpdateCommandTest, ValidateShardVersionAndDatabaseVersion) {
    auto update = BSON("q" << BSONObj() << "u" << BSON("$set" << BSON("_id" << 1)));
    {
        auto mirroredObj = createCommandAndGetMirrored(kCollection, {update});

        ASSERT_FALSE(mirroredObj.hasField("shardVersion"));
        ASSERT_FALSE(mirroredObj.hasField("databaseVersion"));
    }

    shardVersion = BSON("shardVersion" << kShardVersion.toBSON());
    databaseVersion = BSON("databaseVersion" << kDatabaseVersion.toBSON());
    {
        auto mirroredObj = createCommandAndGetMirrored(kCollection, {update});

        ASSERT_TRUE(mirroredObj.hasField("shardVersion"));
        ASSERT_TRUE(mirroredObj.hasField("databaseVersion"));
        ASSERT_BSONOBJ_EQ(mirroredObj["shardVersion"].Obj(), kShardVersion.toBSON());
        ASSERT_BSONOBJ_EQ(mirroredObj["databaseVersion"].Obj(), kDatabaseVersion.toBSON());
    }
}

TEST_F(UpdateCommandTest, ValidateEncryptionInformation) {
    auto update = BSON("q" << BSONObj() << "u" << BSON("$set" << BSON("_id" << 1)));
    {
        auto mirroredObj = createCommandAndGetMirrored(kCollection, {update});
        ASSERT_FALSE(mirroredObj.hasField("encryptionInformation"));
    }

    const auto encInfoValue = BSON("type" << 1 << "schema" << BSONObj::kEmptyObject);
    encryptionInformation = BSON("encryptionInformation" << encInfoValue);
    {
        auto mirroredObj = createCommandAndGetMirrored(kCollection, {update});

        ASSERT_TRUE(mirroredObj.hasField("encryptionInformation"));
        ASSERT(compareBSONObjs(mirroredObj["encryptionInformation"].Obj(), encInfoValue));
    }
}

TEST_F(UpdateCommandTest, ValidateRawData) {
    auto update =
        BSON("q" << BSON("control.count" << 2) << "u" << BSON("$set" << BSON("meta" << 3)));

    {
        auto mirroredObj = createCommandAndGetMirrored(kCollection, {update});
        ASSERT_FALSE(mirroredObj.hasField("rawData"));
    }

    rawData = BSON("rawData" << true);
    {
        auto mirroredObj = createCommandAndGetMirrored(kCollection, {update});
        ASSERT_TRUE(mirroredObj.hasField("rawData"));
        ASSERT_TRUE(mirroredObj["rawData"].Bool());
    }
}

class BulkWriteTest : public CommandMirroringTest {
public:
    std::string commandName() override {
        return "bulkWrite";
    }

private:
    RAIIServerParameterControllerForTest controller{"featureFlagBulkWriteCommand", true};
};

TEST_F(BulkWriteTest, NoUpdateOp) {
    auto bulkWriteArgs = {
        BSON("ops" << BSON_ARRAY(BSON("insert" << 0 << "document" << BSON("_id" << 1))
                                 << BSON("delete" << 0 << "filter" << BSON("_id" << 2)))),
        BSON("nsInfo" << BSON_ARRAY(BSON("ns" << kNss)))};

    ASSERT_THROWS_CODE(createCommandAndGetMirrored("1", bulkWriteArgs),
                       DBException,
                       ErrorCodes::CommandNotSupported);
}

TEST_F(BulkWriteTest, NoQueryInUpdateOp) {
    auto bulkWriteArgs = {
        BSON("ops" << BSON_ARRAY(BSON("insert" << 0 << "document" << BSON("_id" << 1))
                                 << BSON("update" << 0 << "filter" << BSONObj() << "updateMods"
                                                  << BSON("$set" << BSON("_id" << 1))))),
        BSON("nsInfo" << BSON_ARRAY(BSON("ns" << kNss)))};

    auto mirroredObj = createCommandAndGetMirrored("1", bulkWriteArgs);

    ASSERT_EQ(mirroredObj["find"].String(), kCollection);
    ASSERT_FALSE(mirroredObj.hasField("filter"));
    ASSERT_FALSE(mirroredObj.hasField("hint"));
    ASSERT_TRUE(mirroredObj["singleBatch"].Bool());
    ASSERT_EQ(mirroredObj["batchSize"].Int(), 1);
    ASSERT_FALSE(mirroredObj.hasField("shardVersion"));
    ASSERT_FALSE(mirroredObj.hasField("databaseVersion"));
}

TEST_F(BulkWriteTest, SingleQueryInUpdateOp) {
    auto bulkWriteArgs = {
        BSON("ops" << BSON_ARRAY(BSON("insert" << 0 << "document" << BSON("_id" << 1))
                                 << BSON("update"
                                         << 0 << "filter" << BSON("qty" << BSON("$lt" << 50.0))
                                         << "updateMods" << BSON("$inc" << BSON("qty" << 1))))),
        BSON("nsInfo" << BSON_ARRAY(BSON("ns" << kNss)))};

    auto mirroredObj = createCommandAndGetMirrored("1", bulkWriteArgs);

    ASSERT_EQ(mirroredObj["find"].String(), kCollection);
    ASSERT_EQ(mirroredObj["filter"].Obj().toString(), "{ qty: { $lt: 50.0 } }");
    ASSERT_FALSE(mirroredObj.hasField("hint"));
    ASSERT_TRUE(mirroredObj["singleBatch"].Bool());
    ASSERT_EQ(mirroredObj["batchSize"].Int(), 1);
    ASSERT_FALSE(mirroredObj.hasField("shardVersion"));
    ASSERT_FALSE(mirroredObj.hasField("databaseVersion"));
}

TEST_F(BulkWriteTest, SingleQueryInUpdateOpWithHintCollationSort) {
    auto bulkWriteArgs = {
        BSON("ops" << BSON_ARRAY(
                 BSON("insert" << 0 << "document" << BSON("_id" << 1))
                 << BSON("update" << 0 << "filter" << BSON("price" << BSON("$gt" << 100))
                                  << "updateMods" << BSON("$inc" << BSON("price" << 1)) << "hint"
                                  << BSON("price" << 1) << "collation" << BSON("locale" << "fr")
                                  << "sort" << BSON("price" << 1)))),
        BSON("nsInfo" << BSON_ARRAY(BSON("ns" << kNss)))};

    auto mirroredObj = createCommandAndGetMirrored("1", bulkWriteArgs);

    ASSERT_EQ(mirroredObj["find"].String(), kCollection);
    ASSERT_EQ(mirroredObj["filter"].Obj().toString(), "{ price: { $gt: 100 } }");
    ASSERT_EQ(mirroredObj["hint"].Obj().toString(), "{ price: 1 }");
    ASSERT_EQ(mirroredObj["collation"].Obj().toString(), "{ locale: \"fr\" }");
    ASSERT_EQ(mirroredObj["sort"].Obj().toString(), "{ price: 1 }");
    ASSERT_TRUE(mirroredObj["singleBatch"].Bool());
    ASSERT_EQ(mirroredObj["batchSize"].Int(), 1);
    ASSERT_FALSE(mirroredObj.hasField("shardVersion"));
    ASSERT_FALSE(mirroredObj.hasField("databaseVersion"));
}

TEST_F(BulkWriteTest, MultipleUpdateOpsAndNamespaces) {
    const std::string kCollection2 = "testColl2";
    const std::string kNss2 = kDB + "." + kCollection2;

    auto bulkWriteArgs = {
        BSON("ops" << BSON_ARRAY(
                 BSON("delete" << 0 << "filter" << BSON("_id" << 1))
                 << BSON("update" << 1 << "filter" << BSON("_id" << BSON("$eq" << 1))
                                  << "updateMods" << BSON("$inc" << BSON("qty" << 1)))
                 << BSON("update" << 0 << "filter" << BSON("_id" << BSON("$eq" << 0))
                                  << "updateMods" << BSON("$inc" << BSON("qty" << -1))))),
        BSON("nsInfo" << BSON_ARRAY(BSON("ns" << kNss) << BSON("ns" << kNss2)))};

    auto mirroredObj = createCommandAndGetMirrored("1", bulkWriteArgs);

    ASSERT_EQ(mirroredObj["find"].String(), kCollection2);
    ASSERT_EQ(mirroredObj["filter"].Obj().toString(), "{ _id: { $eq: 1 } }");
    ASSERT_FALSE(mirroredObj.hasField("hint"));
    ASSERT_TRUE(mirroredObj["singleBatch"].Bool());
    ASSERT_EQ(mirroredObj["batchSize"].Int(), 1);
    ASSERT_FALSE(mirroredObj.hasField("shardVersion"));
    ASSERT_FALSE(mirroredObj.hasField("databaseVersion"));
}

TEST_F(BulkWriteTest, ValidateShardVersionAndDatabaseVersion) {
    auto bulkWriteArgs = {
        BSON("ops" << BSON_ARRAY(BSON("insert" << 0 << "document" << BSON("_id" << 1))
                                 << BSON("update"
                                         << 0 << "filter" << BSON("qty" << BSON("$lt" << 50.0))
                                         << "updateMods" << BSON("$inc" << BSON("qty" << 1))))),
        BSON("nsInfo" << BSON_ARRAY(BSON("ns" << kNss << "shardVersion" << kShardVersion.toBSON()
                                              << "databaseVersion" << kDatabaseVersion.toBSON())))};

    auto mirroredObj = createCommandAndGetMirrored("1", bulkWriteArgs);

    ASSERT_EQ(mirroredObj["find"].String(), kCollection);
    ASSERT_EQ(mirroredObj["filter"].Obj().toString(), "{ qty: { $lt: 50.0 } }");
    ASSERT_FALSE(mirroredObj.hasField("hint"));
    ASSERT_TRUE(mirroredObj["singleBatch"].Bool());
    ASSERT_EQ(mirroredObj["batchSize"].Int(), 1);
    ASSERT_TRUE(mirroredObj.hasField("shardVersion"));
    ASSERT_TRUE(mirroredObj.hasField("databaseVersion"));
    ASSERT_BSONOBJ_EQ(mirroredObj["shardVersion"].Obj(), kShardVersion.toBSON());
    ASSERT_BSONOBJ_EQ(mirroredObj["databaseVersion"].Obj(), kDatabaseVersion.toBSON());
}

TEST_F(BulkWriteTest, ValidateEncryptionInformation) {
    const auto encInfoValue = BSON("type" << 1 << "schema" << BSONObj::kEmptyObject);
    const auto encryptionInformation = BSON("encryptionInformation" << encInfoValue);
    const auto bulkWriteArgs = {
        BSON("ops" << BSON_ARRAY(BSON("update" << 0 << "filter" << BSON("_id" << 0) << "updateMods"
                                               << BSON("$inc" << BSON("qty" << -1))))),
        BSON(
            "nsInfo" << BSON_ARRAY(BSON("ns" << kNss << "encryptionInformation" << encInfoValue)))};

    auto mirroredObj = createCommandAndGetMirrored("1", bulkWriteArgs);

    ASSERT_TRUE(mirroredObj.hasField("encryptionInformation"));
    ASSERT(compareBSONObjs(mirroredObj["encryptionInformation"].Obj(), encInfoValue));
}

TEST_F(BulkWriteTest, ValidateRawData) {
    const auto bulkWriteArgs = {
        BSON("ops" << BSON_ARRAY(BSON("update" << 0 << "filter" << BSON("_id" << 0) << "updateMods"
                                               << BSON("$inc" << BSON("qty" << -1))))),
        BSON("nsInfo" << BSON_ARRAY(BSON("ns" << kNss))),
        BSON("rawData" << true)};

    auto mirroredObj = createCommandAndGetMirrored(kCollection, {bulkWriteArgs});
    ASSERT_TRUE(mirroredObj.hasField("rawData"));
    ASSERT_TRUE(mirroredObj["rawData"].Bool());
}

class FindCommandTest : public CommandMirroringTest {
public:
    std::string commandName() override {
        return "find";
    }

    virtual std::vector<std::string> getAllowedKeys() const {
        return {
            "find",
            "filter",
            "skip",
            "limit",
            "sort",
            "hint",
            "collation",
            "min",
            "max",
            "batchSize",
            "singleBatch",
            "shardVersion",
            "databaseVersion",
            "encryptionInformation",
            "rawData",
        };
    }

    void checkFieldNamesAreAllowed(BSONObj& mirroredObj) {
        const auto possibleKeys = getAllowedKeys();
        for (const auto& key : mirroredObj.getFieldNames<std::set<std::string>>()) {
            ASSERT(std::find(possibleKeys.begin(), possibleKeys.end(), key) != possibleKeys.end());
        }
    }
};

TEST_F(FindCommandTest, MirrorableKeys) {
    auto findArgs = {
        BSON("filter" << BSONObj()),
        BSON("sort" << BSONObj()),
        BSON("projection" << BSONObj()),
        BSON("hint" << BSONObj()),
        BSON("skip" << 1),
        BSON("limit" << 1),
        BSON("batchSize" << 1),
        BSON("singleBatch" << false),
        BSON("comment" << "This is a comment."),
        BSON("maxTimeMS" << 100),
        BSON("readConcern" << BSON("level" << "local")),
        BSON("max" << BSONObj()),
        BSON("min" << BSONObj()),
        BSON("returnKey" << true),
        BSON("showRecordId" << false),
        BSON("tailable" << true),
        BSON("oplogReplay" << true),
        BSON("noCursorTimeout" << true),
        BSON("awaitData" << true),
        BSON("allowPartialResults" << true),
        BSON("collation" << BSONObj()),
        BSON("shardVersion" << kShardVersion.toBSON()),
        BSON("databaseVersion" << kDatabaseVersion.toBSON()),
        BSON("encryptionInformation" << BSON("schema" << BSONObj::kEmptyObject)),
        BSON("rawData" << true),
    };

    auto mirroredObj = createCommandAndGetMirrored(kCollection, findArgs);
    checkFieldNamesAreAllowed(mirroredObj);
}

TEST_F(FindCommandTest, BatchSizeReconfiguration) {
    auto findArgs = {
        BSON("filter" << BSONObj()), BSON("batchSize" << 100), BSON("singleBatch" << false)};

    auto mirroredObj = createCommandAndGetMirrored(kCollection, findArgs);
    ASSERT_EQ(mirroredObj["singleBatch"].Bool(), true);
    ASSERT_EQ(mirroredObj["batchSize"].Int(), 1);
}

TEST_F(FindCommandTest, ValidateMirroredQuery) {
    const auto filter = BSON("rating" << BSON("$gte" << 9) << "cuisine"
                                      << "Italian");
    constexpr auto skip = 10;
    constexpr auto limit = 50;
    const auto sortObj = BSON("name" << 1);
    const auto hint = BSONObj();
    const auto collation = BSON("locale" << "\"fr\""
                                         << "strength" << 1);
    const auto min = BSONObj();
    const auto max = BSONObj();

    const auto encryptionInformation = BSON("type" << 1 << "schema" << BSONObj::kEmptyObject);
    const auto rawData = true;

    auto findArgs = {
        BSON("filter" << filter),
        BSON("skip" << skip),
        BSON("limit" << limit),
        BSON("sort" << sortObj),
        BSON("hint" << hint),
        BSON("collation" << collation),
        BSON("min" << min),
        BSON("max" << max),
        BSON("shardVersion" << kShardVersion.toBSON()),
        BSON("databaseVersion" << kDatabaseVersion.toBSON()),
        BSON("encryptionInformation" << encryptionInformation),
        BSON("rawData" << rawData),
    };

    auto mirroredObj = createCommandAndGetMirrored(kCollection, findArgs);

    ASSERT_EQ(mirroredObj["find"].String(), kCollection);
    ASSERT(compareBSONObjs(mirroredObj["filter"].Obj(), filter));
    ASSERT_EQ(mirroredObj["skip"].Int(), skip);
    ASSERT_EQ(mirroredObj["limit"].Int(), limit);
    ASSERT(compareBSONObjs(mirroredObj["sort"].Obj(), sortObj));
    ASSERT(compareBSONObjs(mirroredObj["hint"].Obj(), hint));
    ASSERT(compareBSONObjs(mirroredObj["collation"].Obj(), collation));
    ASSERT(compareBSONObjs(mirroredObj["min"].Obj(), min));
    ASSERT(compareBSONObjs(mirroredObj["max"].Obj(), max));
    ASSERT(compareBSONObjs(mirroredObj["shardVersion"].Obj(), kShardVersion.toBSON()));
    ASSERT(compareBSONObjs(mirroredObj["databaseVersion"].Obj(), kDatabaseVersion.toBSON()));
    ASSERT(compareBSONObjs(mirroredObj["encryptionInformation"].Obj(), encryptionInformation));
    ASSERT_EQ(mirroredObj["rawData"].Bool(), rawData);
}

TEST_F(FindCommandTest, ValidateShardVersionAndDatabaseVersion) {
    std::vector<BSONObj> findArgs = {BSON("filter" << BSONObj())};
    {
        auto mirroredObj = createCommandAndGetMirrored(kCollection, findArgs);
        ASSERT_FALSE(mirroredObj.hasField("shardVersion"));
        ASSERT_FALSE(mirroredObj.hasField("databaseVersion"));
    }

    findArgs.push_back(BSON("shardVersion" << kShardVersion.toBSON()));
    findArgs.push_back(BSON("databaseVersion" << kDatabaseVersion.toBSON()));
    {
        auto mirroredObj = createCommandAndGetMirrored(kCollection, findArgs);
        ASSERT_TRUE(mirroredObj.hasField("shardVersion"));
        ASSERT_TRUE(mirroredObj.hasField("databaseVersion"));
        ASSERT_BSONOBJ_EQ(mirroredObj["shardVersion"].Obj(), kShardVersion.toBSON());
        ASSERT_BSONOBJ_EQ(mirroredObj["databaseVersion"].Obj(), kDatabaseVersion.toBSON());
    }
}

class FindAndModifyCommandTest : public FindCommandTest {
public:
    std::string commandName() override {
        return "findAndModify";
    }

    std::vector<std::string> getAllowedKeys() const override {
        return {
            "sort",
            "collation",
            "find",
            "filter",
            "batchSize",
            "singleBatch",
            "shardVersion",
            "databaseVersion",
            "encryptionInformation",
            "rawData",
        };
    }
};

TEST_F(FindAndModifyCommandTest, MirrorableKeys) {
    auto findAndModifyArgs = {
        BSON("query" << BSONObj()),
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
        BSON("arrayFilters" << BSONArray()),
        BSON("shardVersion" << kShardVersion.toBSON()),
        BSON("databaseVersion" << kDatabaseVersion.toBSON()),
        BSON("encryptionInformation" << BSON("type" << 1 << "schema" << BSONObj::kEmptyObject)),
        BSON("rawData" << true),
    };

    auto mirroredObj = createCommandAndGetMirrored(kCollection, findAndModifyArgs);
    checkFieldNamesAreAllowed(mirroredObj);
}

TEST_F(FindAndModifyCommandTest, BatchSizeReconfiguration) {
    auto findAndModifyArgs = {BSON("query" << BSONObj()),
                              BSON("update" << BSONObj()),
                              BSON("batchSize" << 100),
                              BSON("singleBatch" << false)};

    auto mirroredObj = createCommandAndGetMirrored(kCollection, findAndModifyArgs);
    ASSERT_EQ(mirroredObj["singleBatch"].Bool(), true);
    ASSERT_EQ(mirroredObj["batchSize"].Int(), 1);
}

TEST_F(FindAndModifyCommandTest, ValidateMirroredQuery) {
    const auto query = BSON("name" << "Andy");
    const auto sortObj = BSON("rating" << 1);
    const auto update = BSON("$inc" << BSON("score" << 1));
    constexpr auto upsert = true;
    const auto collation = BSON("locale" << "\"fr\"");
    const auto encInfoValue = BSON("type" << 1 << "schema" << BSONObj::kEmptyObject);
    const auto rawData = true;

    auto findAndModifyArgs = {
        BSON("query" << query),
        BSON("sort" << sortObj),
        BSON("update" << update),
        BSON("upsert" << upsert),
        BSON("collation" << collation),
        BSON("shardVersion" << kShardVersion.toBSON()),
        BSON("databaseVersion" << kDatabaseVersion.toBSON()),
        BSON("encryptionInformation" << encInfoValue),
        BSON("rawData" << rawData),
    };

    auto mirroredObj = createCommandAndGetMirrored(kCollection, findAndModifyArgs);

    ASSERT_EQ(mirroredObj["find"].String(), kCollection);
    ASSERT(!mirroredObj.hasField("upsert"));
    ASSERT(compareBSONObjs(mirroredObj["filter"].Obj(), query));
    ASSERT(compareBSONObjs(mirroredObj["sort"].Obj(), sortObj));
    ASSERT(compareBSONObjs(mirroredObj["collation"].Obj(), collation));
    ASSERT(compareBSONObjs(mirroredObj["shardVersion"].Obj(), kShardVersion.toBSON()));
    ASSERT(compareBSONObjs(mirroredObj["databaseVersion"].Obj(), kDatabaseVersion.toBSON()));
    ASSERT(compareBSONObjs(mirroredObj["encryptionInformation"].Obj(), encInfoValue));
    ASSERT_EQ(mirroredObj["rawData"].Bool(), rawData);
}

TEST_F(FindAndModifyCommandTest, ValidateShardVersionAndDatabaseVersion) {
    std::vector<BSONObj> findAndModifyArgs = {BSON("query" << BSON("name" << "Andy")),
                                              BSON("update" << BSON("$inc" << BSON("score" << 1)))};

    {
        auto mirroredObj = createCommandAndGetMirrored(kCollection, findAndModifyArgs);
        ASSERT_FALSE(mirroredObj.hasField("shardVersion"));
        ASSERT_FALSE(mirroredObj.hasField("databaseVersion"));
    }

    findAndModifyArgs.push_back(BSON("shardVersion" << kShardVersion.toBSON()));
    findAndModifyArgs.push_back(BSON("databaseVersion" << kDatabaseVersion.toBSON()));
    {
        auto mirroredObj = createCommandAndGetMirrored(kCollection, findAndModifyArgs);
        ASSERT_TRUE(mirroredObj.hasField("shardVersion"));
        ASSERT_TRUE(mirroredObj.hasField("databaseVersion"));
        ASSERT_BSONOBJ_EQ(mirroredObj["shardVersion"].Obj(), kShardVersion.toBSON());
        ASSERT_BSONOBJ_EQ(mirroredObj["databaseVersion"].Obj(), kDatabaseVersion.toBSON());
    }
}

class DistinctCommandTest : public FindCommandTest {
public:
    std::string commandName() override {
        return "distinct";
    }

    std::vector<std::string> getAllowedKeys() const override {
        return {
            "distinct",
            "key",
            "query",
            "collation",
            "shardVersion",
            "databaseVersion",
            "rawData",
        };
    }
};

TEST_F(DistinctCommandTest, MirrorableKeys) {
    auto distinctArgs = {
        BSON("key" << ""),
        BSON("query" << BSONObj()),
        BSON("readConcern" << BSONObj()),
        BSON("collation" << BSONObj()),
        BSON("shardVersion" << kShardVersion.toBSON()),
        BSON("databaseVersion" << kDatabaseVersion.toBSON()),
        BSON("rawData" << true),
    };

    auto mirroredObj = createCommandAndGetMirrored(kCollection, distinctArgs);
    checkFieldNamesAreAllowed(mirroredObj);
}

TEST_F(DistinctCommandTest, ValidateMirroredQuery) {
    constexpr auto key = "rating";
    const auto query = BSON("cuisine" << "italian");
    const auto readConcern = BSON("level" << "majority");
    const auto collation = BSON("strength" << 1);
    const auto rawData = true;

    auto distinctArgs = {
        BSON("key" << key),
        BSON("query" << query),
        BSON("readConcern" << readConcern),
        BSON("collation" << collation),
        BSON("shardVersion" << kShardVersion.toBSON()),
        BSON("databaseVersion" << kDatabaseVersion.toBSON()),
        BSON("rawData" << rawData),
    };

    auto mirroredObj = createCommandAndGetMirrored(kCollection, distinctArgs);

    ASSERT_EQ(mirroredObj["distinct"].String(), kCollection);
    ASSERT(!mirroredObj.hasField("readConcern"));
    ASSERT_EQ(mirroredObj["key"].String(), key);
    ASSERT(compareBSONObjs(mirroredObj["query"].Obj(), query));
    ASSERT(compareBSONObjs(mirroredObj["collation"].Obj(), collation));
    ASSERT(compareBSONObjs(mirroredObj["shardVersion"].Obj(), kShardVersion.toBSON()));
    ASSERT(compareBSONObjs(mirroredObj["databaseVersion"].Obj(), kDatabaseVersion.toBSON()));
    ASSERT_EQ(mirroredObj["rawData"].Bool(), rawData);
}

TEST_F(DistinctCommandTest, ValidateShardVersionAndDatabaseVersion) {
    std::vector<BSONObj> distinctArgs = {};
    {
        auto mirroredObj = createCommandAndGetMirrored(kCollection, distinctArgs);
        ASSERT_FALSE(mirroredObj.hasField("shardVersion"));
        ASSERT_FALSE(mirroredObj.hasField("databaseVersion"));
    }

    distinctArgs.push_back(BSON("shardVersion" << kShardVersion.toBSON()));
    distinctArgs.push_back(BSON("databaseVersion" << kDatabaseVersion.toBSON()));
    {
        auto mirroredObj = createCommandAndGetMirrored(kCollection, distinctArgs);
        ASSERT_TRUE(mirroredObj.hasField("shardVersion"));
        ASSERT_TRUE(mirroredObj.hasField("databaseVersion"));
        ASSERT_BSONOBJ_EQ(mirroredObj["shardVersion"].Obj(), kShardVersion.toBSON());
        ASSERT_BSONOBJ_EQ(mirroredObj["databaseVersion"].Obj(), kDatabaseVersion.toBSON());
    }
}

class CountCommandTest : public FindCommandTest {
public:
    std::string commandName() override {
        return "count";
    }

    std::vector<std::string> getAllowedKeys() const override {
        return {
            "count",
            "query",
            "skip",
            "limit",
            "hint",
            "collation",
            "shardVersion",
            "databaseVersion",
            "encryptionInformation",
            "rawData",
        };
    }
};

TEST_F(CountCommandTest, MirrorableKeys) {
    auto countArgs = {
        BSON("query" << BSONObj()),
        BSON("limit" << 100),
        BSON("skip" << 10),
        BSON("hint" << BSONObj()),
        BSON("readConcern" << BSONObj()),
        BSON("collation" << BSONObj()),
        BSON("shardVersion" << kShardVersion.toBSON()),
        BSON("databaseVersion" << kDatabaseVersion.toBSON()),
        BSON("encryptionInformation" << BSON("schema" << BSONObj::kEmptyObject)),
        BSON("rawData" << true),
    };

    auto mirroredObj = createCommandAndGetMirrored(kCollection, countArgs);
    checkFieldNamesAreAllowed(mirroredObj);
}

TEST_F(CountCommandTest, ValidateMirroredQuery) {
    const auto query = BSON("status" << "Delivered");
    const auto hint = BSON("status" << 1);
    constexpr auto limit = 1000;
    const auto encInfoValue = BSON("type" << 1 << "schema" << BSONObj::kEmptyObject);
    const auto rawData = true;

    auto countArgs = {
        BSON("query" << query),
        BSON("hint" << hint),
        BSON("limit" << limit),
        BSON("shardVersion" << kShardVersion.toBSON()),
        BSON("databaseVersion" << kDatabaseVersion.toBSON()),
        BSON("encryptionInformation" << encInfoValue),
        BSON("rawData" << rawData),
    };
    auto mirroredObj = createCommandAndGetMirrored(kCollection, countArgs);

    ASSERT_EQ(mirroredObj["count"].String(), kCollection);
    ASSERT(!mirroredObj.hasField("skip"));
    ASSERT(!mirroredObj.hasField("collation"));
    ASSERT(compareBSONObjs(mirroredObj["query"].Obj(), query));
    ASSERT(compareBSONObjs(mirroredObj["hint"].Obj(), hint));
    ASSERT_EQ(mirroredObj["limit"].Long(), limit);
    ASSERT(compareBSONObjs(mirroredObj["shardVersion"].Obj(), kShardVersion.toBSON()));
    ASSERT(compareBSONObjs(mirroredObj["databaseVersion"].Obj(), kDatabaseVersion.toBSON()));
    ASSERT(compareBSONObjs(mirroredObj["encryptionInformation"].Obj(), encInfoValue));
    ASSERT_EQ(mirroredObj["rawData"].Bool(), rawData);
}

TEST_F(CountCommandTest, ValidateShardVersionAndDatabaseVersion) {
    std::vector<BSONObj> countArgs = {};
    {
        auto mirroredObj = createCommandAndGetMirrored(kCollection, countArgs);
        ASSERT_FALSE(mirroredObj.hasField("shardVersion"));
        ASSERT_FALSE(mirroredObj.hasField("databaseVersion"));
    }

    countArgs.push_back(BSON("shardVersion" << kShardVersion.toBSON()));
    countArgs.push_back(BSON("databaseVersion" << kDatabaseVersion.toBSON()));
    {
        auto mirroredObj = createCommandAndGetMirrored(kCollection, countArgs);
        ASSERT_TRUE(mirroredObj.hasField("shardVersion"));
        ASSERT_TRUE(mirroredObj.hasField("databaseVersion"));
        ASSERT_BSONOBJ_EQ(mirroredObj["shardVersion"].Obj(), kShardVersion.toBSON());
        ASSERT_BSONOBJ_EQ(mirroredObj["databaseVersion"].Obj(), kDatabaseVersion.toBSON());
    }
}

}  // namespace
}  // namespace mongo
