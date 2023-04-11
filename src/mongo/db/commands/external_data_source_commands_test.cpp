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

#include <fmt/format.h>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/dbclient_cursor.h"
#include "mongo/db/database_name.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/pipeline/aggregation_request_helper.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/storage/named_pipe.h"
#include "mongo/platform/random.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/scopeguard.h"

namespace mongo {
namespace {
using namespace fmt::literals;

class PipeWaiter {
public:
    void notify() {
        {
            stdx::unique_lock lk(m);
            pipeCreated = true;
        }
        cv.notify_one();
    }

    void wait() {
        stdx::unique_lock lk(m);
        cv.wait(lk, [&] { return pipeCreated; });
    }

private:
    Mutex m;
    stdx::condition_variable cv;
    bool pipeCreated = false;
};

class ExternalDataSourceCommandsTest : public ServiceContextMongoDTest {
protected:
    void setUp() override {
        ServiceContextMongoDTest::setUp();

        const auto service = getServiceContext();
        auto replCoord =
            std::make_unique<repl::ReplicationCoordinatorMock>(service, repl::ReplSettings{});
        ASSERT_OK(replCoord->setFollowerMode(repl::MemberState::RS_PRIMARY));
        repl::ReplicationCoordinator::set(service, std::move(replCoord));
        repl::createOplog(_opCtx);

        computeModeEnabled = true;
    }

    void tearDown() override {
        computeModeEnabled = false;
        ServiceContextMongoDTest::tearDown();
    }

    std::vector<BSONObj> generateRandomSimpleDocs(int count) {
        std::vector<BSONObj> docs;
        for (int i = 0; i < count; ++i) {
            docs.emplace_back(BSON("a" << _random.nextInt32(10)));
        }

        return docs;
    }

    // Generates a large readable random string to aid debugging.
    std::string getRandomReadableLargeString() {
        int count = _random.nextInt32(100) + 2024;
        std::string str(count, '\0');
        for (int i = 0; i < count; ++i) {
            str[i] = static_cast<char>(_random.nextInt32(26)) + 'a';
        }

        return str;
    }

    std::vector<BSONObj> generateRandomLargeDocs(int count) {
        std::vector<BSONObj> docs;
        for (int i = 0; i < count; ++i) {
            docs.emplace_back(BSON("a" << getRandomReadableLargeString()));
        }

        return docs;
    }

    // This verifies that a simple aggregate command works with explain:true. Virtual collections
    // are created even for explain aggregate command.
    void verifyExplainAggCommand(DBDirectClient& client, const BSONObj& originalAggCommand) {
        // The first request.
        BSONObj res;
        ASSERT_TRUE(client.runCommand(
            kDatabaseName, originalAggCommand.addFields(BSON("explain" << true)), res))
            << "Expected to succeed but failed. result = {}"_format(res.toString());
        // Sanity checks of result.
        ASSERT_EQ(res["ok"].Number(), 1.0)
            << "Expected to succeed but failed. result = {}"_format(res.toString());
    }

    PseudoRandom _random{SecureRandom{}.nextInt64()};
    ServiceContext::UniqueOperationContext _uniqueOpCtx{makeOperationContext()};
    OperationContext* _opCtx{_uniqueOpCtx.get()};

    static const DatabaseName kDatabaseName;
};

const DatabaseName ExternalDataSourceCommandsTest::kDatabaseName =
    DatabaseName::createDatabaseName_forTest(boost::none, "external_data_source");

TEST_F(ExternalDataSourceCommandsTest, SimpleScanAggRequest) {
    const auto nDocs = _random.nextInt32(100) + 1;
    std::vector<BSONObj> srcDocs = generateRandomSimpleDocs(nDocs);
    PipeWaiter pw;

    stdx::thread producer([&] {
        NamedPipeOutput pipeWriter("EDSCTest_SimpleScanAggRequestPipe");
        pw.notify();
        pipeWriter.open();
        for (auto&& srcDoc : srcDocs) {
            pipeWriter.write(srcDoc.objdata(), srcDoc.objsize());
        }
        pipeWriter.close();
    });
    ON_BLOCK_EXIT([&] { producer.join(); });

    // Gives some time to the producer so that it can initialize a named pipe.
    pw.wait();

    DBDirectClient client(_opCtx);
    auto aggCmdObj = fromjson(R"(
{
    aggregate: "coll",
    pipeline: [],
    cursor: {},
    $_externalDataSources: [{
        collName: "coll",
        dataSources: [{url: "file://EDSCTest_SimpleScanAggRequestPipe", storageType: "pipe", fileType: "bson"}]
    }]
}
    )");

    // The first request.
    BSONObj res;
    ASSERT_TRUE(client.runCommand(kDatabaseName, aggCmdObj.getOwned(), res));

    // Sanity checks of result.
    ASSERT_EQ(res["ok"].Number(), 1.0);
    ASSERT_TRUE(res.hasField("cursor") && res["cursor"].Obj().hasField("firstBatch"));

    // The default batch size is 101 and so all data must be contained in the first batch. cursor.id
    // == 0 means that no cursor is necessary.
    ASSERT_TRUE(res["cursor"].Obj().hasField("id") && res["cursor"]["id"].Long() == 0);
    auto resDocs = res["cursor"]["firstBatch"].Array();
    ASSERT_EQ(resDocs.size(), nDocs);
    for (int i = 0; i < nDocs; ++i) {
        ASSERT_BSONOBJ_EQ(resDocs[i].Obj(), srcDocs[i]);
    }

    // The second request. This verifies that virtual collections are cleaned up after the
    // aggregation request is done.
    verifyExplainAggCommand(client, aggCmdObj);
}

TEST_F(ExternalDataSourceCommandsTest, SimpleScanOverMultipleNamedPipesAggRequest) {
    // This data set fits into the first batch.
    const auto nDocs = _random.nextInt32(50);
    std::vector<BSONObj> srcDocs = generateRandomSimpleDocs(nDocs);
    PipeWaiter pw;

    // Pushes data into multiple named pipes. We can't push data into multiple named pipes
    // simultaneously because writers will be blocked until the reader consumes data. So, we push
    // data into one named pipe after another.
    stdx::thread producer([&] {
        NamedPipeOutput pipeWriter1("EDSCTest_SimpleScanOverMultipleNamedPipesAggRequestPipe1");
        NamedPipeOutput pipeWriter2("EDSCTest_SimpleScanOverMultipleNamedPipesAggRequestPipe2");
        pw.notify();
        pipeWriter1.open();
        for (auto&& srcDoc : srcDocs) {
            pipeWriter1.write(srcDoc.objdata(), srcDoc.objsize());
        }
        pipeWriter1.close();

        pipeWriter2.open();
        for (auto&& srcDoc : srcDocs) {
            pipeWriter2.write(srcDoc.objdata(), srcDoc.objsize());
        }
        pipeWriter2.close();
    });
    ON_BLOCK_EXIT([&] { producer.join(); });

    // Gives some time to the producer so that it can initialize a named pipe.
    pw.wait();

    DBDirectClient client(_opCtx);
    auto aggCmdObj = fromjson(R"(
{
    aggregate: "coll",
    pipeline: [],
    cursor: {},
    $_externalDataSources: [{
        collName: "coll",
        dataSources: [
            {url: "file://EDSCTest_SimpleScanOverMultipleNamedPipesAggRequestPipe1", storageType: "pipe", fileType: "bson"},
            {url: "file://EDSCTest_SimpleScanOverMultipleNamedPipesAggRequestPipe2", storageType: "pipe", fileType: "bson"}
        ]
    }]
}
    )");

    // The first request.
    BSONObj res;
    ASSERT_TRUE(client.runCommand(kDatabaseName, aggCmdObj.getOwned(), res));

    // Sanity checks of result.
    ASSERT_EQ(res["ok"].Number(), 1.0);
    ASSERT_TRUE(res.hasField("cursor") && res["cursor"].Obj().hasField("firstBatch"));

    // The default batch size is 101 and so all data must be contained in the first batch. cursor.id
    // == 0 means that no cursor is necessary.
    ASSERT_TRUE(res["cursor"].Obj().hasField("id") && res["cursor"]["id"].Long() == 0);
    auto resDocs = res["cursor"]["firstBatch"].Array();
    ASSERT_EQ(resDocs.size(), nDocs * 2);
    for (int i = 0; i < nDocs; ++i) {
        ASSERT_BSONOBJ_EQ(resDocs[i].Obj(), srcDocs[i % nDocs]);
    }

    // The second request. This verifies that virtual collections are cleaned up after the
    // aggregation request is done.
    verifyExplainAggCommand(client, aggCmdObj);
}

TEST_F(ExternalDataSourceCommandsTest, SimpleScanOverLargeObjectsAggRequest) {
    // MultiBsonStreamCursor's default buffer size is 8K and 2K (at minimum) * 20 would be enough to
    // exceed the initial read. This data set is highly likely to span multiple reads.
    const auto nDocs = _random.nextInt32(80) + 20;
    std::vector<BSONObj> srcDocs = generateRandomLargeDocs(nDocs);
    PipeWaiter pw;

    stdx::thread producer([&] {
        NamedPipeOutput pipeWriter("EDSCTest_SimpleScanOverLargeObjectsAggRequestPipe");
        pw.notify();
        pipeWriter.open();
        for (auto&& srcDoc : srcDocs) {
            pipeWriter.write(srcDoc.objdata(), srcDoc.objsize());
        }
        pipeWriter.close();
    });
    ON_BLOCK_EXIT([&] { producer.join(); });

    // Gives some time to the producer so that it can initialize a named pipe.
    pw.wait();

    DBDirectClient client(_opCtx);
    auto aggCmdObj = fromjson(R"(
{
    aggregate: "coll",
    pipeline: [],
    cursor: {},
    $_externalDataSources: [{
        collName: "coll",
        dataSources: [{url: "file://EDSCTest_SimpleScanOverLargeObjectsAggRequestPipe", storageType: "pipe", fileType: "bson"}]
    }]
}
    )");

    // The first request.
    BSONObj res;
    ASSERT_TRUE(client.runCommand(kDatabaseName, aggCmdObj.getOwned(), res));

    // Sanity checks of result.
    ASSERT_EQ(res["ok"].Number(), 1.0);
    ASSERT_TRUE(res.hasField("cursor") && res["cursor"].Obj().hasField("firstBatch"));

    // The default batch size is 101 and so all data must be contained in the first batch. cursor.id
    // == 0 means that no cursor is necessary.
    ASSERT_TRUE(res["cursor"].Obj().hasField("id") && res["cursor"]["id"].Long() == 0);
    auto resDocs = res["cursor"]["firstBatch"].Array();
    ASSERT_EQ(resDocs.size(), nDocs);
    for (int i = 0; i < nDocs; ++i) {
        ASSERT_BSONOBJ_EQ(resDocs[i].Obj(), srcDocs[i]);
    }

    // The second request. This verifies that virtual collections are cleaned up after the
    // aggregation request is done.
    verifyExplainAggCommand(client, aggCmdObj);
}

// Tests that 'explain' flag works and also tests that the same aggregation request works with the
// same $_externalDataSources again to see whether there are no remaining virtual collections left
// behind after the aggregation request is done.
TEST_F(ExternalDataSourceCommandsTest, ExplainAggRequest) {
    auto aggCmdObj = fromjson(R"(
{
    aggregate: "coll",
    pipeline: [],
    cursor: {},
    $_externalDataSources: [{
        collName: "coll",
        dataSources: [{url: "file://EDSCTest_ExplainAggRequestPipe", storageType: "pipe", fileType: "bson"}]
    }]
}
    )");

    DBDirectClient client(_opCtx);
    // The first request.
    verifyExplainAggCommand(client, aggCmdObj);

    // The second request.
    verifyExplainAggCommand(client, aggCmdObj);
}

TEST_F(ExternalDataSourceCommandsTest, SimpleScanMultiBatchAggRequest) {
    // This 'nDocs' causes a cursor to be created for a simple scan aggregate command.
    const auto nDocs = _random.nextInt32(100) + 102;
    std::vector<BSONObj> srcDocs = generateRandomSimpleDocs(nDocs);
    PipeWaiter pw;

    stdx::thread producer([&] {
        NamedPipeOutput pipeWriter("EDSCTest_SimpleScanMultiBatchAggRequestPipe");
        pw.notify();
        pipeWriter.open();
        for (auto&& srcDoc : srcDocs) {
            pipeWriter.write(srcDoc.objdata(), srcDoc.objsize());
        }
        pipeWriter.close();
    });
    ON_BLOCK_EXIT([&] { producer.join(); });

    // Gives some time to the producer so that it can initialize a named pipe.
    pw.wait();

    DBDirectClient client(_opCtx);
    auto aggCmdObj = fromjson(R"(
{
    aggregate: "coll",
    pipeline: [],
    cursor: {},
    $_externalDataSources: [{
        collName: "coll",
        dataSources: [{url: "file://EDSCTest_SimpleScanMultiBatchAggRequestPipe", storageType: "pipe", fileType: "bson"}]
    }]
}
    )");

    auto swAggReq = aggregation_request_helper::parseFromBSONForTests(kDatabaseName, aggCmdObj);
    ASSERT_OK(swAggReq.getStatus());
    auto swCursor = DBClientCursor::fromAggregationRequest(
        &client, swAggReq.getValue(), /*secondaryOk*/ false, /*useExhaust*/ false);
    ASSERT_OK(swCursor.getStatus());

    auto cursor = std::move(swCursor.getValue());
    int resCnt = 0;
    // While iterating over the cursor, getMore() request(s) will be sent and the server-side cursor
    // will be destroyed after all data is exhausted.
    while (cursor->more()) {
        auto doc = cursor->next();
        ASSERT_BSONOBJ_EQ(doc, srcDocs[resCnt]);
        ++resCnt;
    }
    ASSERT_EQ(resCnt, nDocs);

    // The second explain request. This verifies that virtual collections are cleaned up after
    // multi-batch result for an aggregation request.
    verifyExplainAggCommand(client, aggCmdObj);
}

TEST_F(ExternalDataSourceCommandsTest, SimpleMatchAggRequest) {
    const auto nDocs = _random.nextInt32(100) + 1;
    std::vector<BSONObj> srcDocs = generateRandomSimpleDocs(nDocs);
    // Expected results for {$match: {a: {$lt: 5}}}.
    std::vector<BSONObj> expectedDocs;
    std::for_each(srcDocs.begin(), srcDocs.end(), [&](const BSONObj& doc) {
        if (doc["a"].Int() < 5) {
            expectedDocs.emplace_back(doc);
        }
    });
    PipeWaiter pw;

    stdx::thread producer([&] {
        NamedPipeOutput pipeWriter("EDSCTest_SimpleMatchAggRequestPipe");
        pw.notify();
        pipeWriter.open();
        for (auto&& srcDoc : srcDocs) {
            pipeWriter.write(srcDoc.objdata(), srcDoc.objsize());
        }
        pipeWriter.close();
    });
    ON_BLOCK_EXIT([&] { producer.join(); });

    // Gives some time to the producer so that it can initialize a named pipe.
    pw.wait();

    DBDirectClient client(_opCtx);
    auto aggCmdObj = fromjson(R"(
{
    aggregate: "coll",
    pipeline: [{$match: {a: {$lt: 5}}}],
    cursor: {},
    $_externalDataSources: [{
        collName: "coll",
        dataSources: [{url: "file://EDSCTest_SimpleMatchAggRequestPipe", storageType: "pipe", fileType: "bson"}]
    }]
}
    )");

    auto swAggReq = aggregation_request_helper::parseFromBSONForTests(kDatabaseName, aggCmdObj);
    ASSERT_OK(swAggReq.getStatus());
    auto swCursor = DBClientCursor::fromAggregationRequest(
        &client, swAggReq.getValue(), /*secondaryOk*/ false, /*useExhaust*/ false);
    ASSERT_OK(swCursor.getStatus());

    auto cursor = std::move(swCursor.getValue());
    int resCnt = 0;
    while (cursor->more()) {
        auto doc = cursor->next();
        ASSERT_BSONOBJ_EQ(doc, expectedDocs[resCnt]);
        ++resCnt;
    }
    ASSERT_EQ(resCnt, expectedDocs.size());

    // The second explain request. This verifies that virtual collections are cleaned up after
    // the aggregation request is done.
    verifyExplainAggCommand(client, aggCmdObj);
}

TEST_F(ExternalDataSourceCommandsTest, KillCursorAfterAggRequest) {
    // This 'nDocs' causes a cursor to be created for a simple scan aggregate command.
    const auto nDocs = _random.nextInt32(100) + 102;
    std::vector<BSONObj> srcDocs = generateRandomSimpleDocs(nDocs);
    PipeWaiter pw;

    stdx::thread producer([&] {
        NamedPipeOutput pipeWriter("EDSCTest_KillCursorAfterAggRequestPipe");
        pw.notify();
        pipeWriter.open();
        for (auto&& srcDoc : srcDocs) {
            pipeWriter.write(srcDoc.objdata(), srcDoc.objsize());
        }
        pipeWriter.close();
    });
    ON_BLOCK_EXIT([&] { producer.join(); });

    // Gives some time to the producer so that it can initialize a named pipe.
    pw.wait();

    DBDirectClient client(_opCtx);
    auto aggCmdObj = fromjson(R"(
{
    aggregate: "coll",
    pipeline: [],
    cursor: {},
    $_externalDataSources: [{
        collName: "coll",
        dataSources: [{url: "file://EDSCTest_KillCursorAfterAggRequestPipe", storageType: "pipe", fileType: "bson"}]
    }]
}
    )");

    // The first request.
    BSONObj res;
    ASSERT_TRUE(client.runCommand(kDatabaseName, aggCmdObj.getOwned(), res));

    // Sanity checks of result.
    ASSERT_EQ(res["ok"].Number(), 1.0);
    ASSERT_TRUE(res.hasField("cursor") && res["cursor"].Obj().hasField("firstBatch"));

    // The default batch size is 101 and results can be returned through multiple batches. cursor.id
    // != 0 means that a cursor is created.
    auto cursorId = res["cursor"]["id"].Long();
    ASSERT_TRUE(res["cursor"].Obj().hasField("id") && cursorId != 0);

    // Kills the cursor.
    auto killCursorCmdObj = BSON("killCursors"
                                 << "coll"
                                 << "cursors" << BSON_ARRAY(cursorId));
    ASSERT_TRUE(client.runCommand(kDatabaseName, killCursorCmdObj.getOwned(), res));
    ASSERT_EQ(res["ok"].Number(), 1.0);
    auto cursorsKilled = res["cursorsKilled"].Array();
    ASSERT_TRUE(cursorsKilled.size() == 1 && cursorsKilled[0].Long() == cursorId);

    // The second explain request. This verifies that virtual collections are cleaned up after
    // the cursor for the aggregate request is killed.
    verifyExplainAggCommand(client, aggCmdObj);
}

TEST_F(ExternalDataSourceCommandsTest, SimpleScanAndUnionWithMultipleSourcesAggRequest) {
    const auto nDocs = _random.nextInt32(100) + 1;
    std::vector<BSONObj> srcDocs = generateRandomSimpleDocs(nDocs);
    PipeWaiter pw;

    stdx::thread producer([&] {
        NamedPipeOutput pipeWriter1(
            "EDSCTest_SimpleScanAndUnionWithMultipleSourcesAggRequestPipe1");
        pw.notify();
        pipeWriter1.open();
        for (auto&& srcDoc : srcDocs) {
            pipeWriter1.write(srcDoc.objdata(), srcDoc.objsize());
        }
        pipeWriter1.close();

        NamedPipeOutput pipeWriter2(
            "EDSCTest_SimpleScanAndUnionWithMultipleSourcesAggRequestPipe2");
        pipeWriter2.open();
        for (auto&& srcDoc : srcDocs) {
            pipeWriter2.write(srcDoc.objdata(), srcDoc.objsize());
        }
        pipeWriter2.close();
    });
    ON_BLOCK_EXIT([&] { producer.join(); });

    // Gives some time to the producer so that it can initialize a named pipe.
    pw.wait();

    DBDirectClient client(_opCtx);
    // An aggregate request with a simple scan and $unionWith stage. $_externalDataSources option
    // defines multiple data sources.
    auto aggCmdObj = fromjson(R"(
{
    aggregate: "coll1",
    pipeline: [{$unionWith: "coll2"}],
    cursor: {},
    $_externalDataSources: [{
        collName: "coll1",
        dataSources: [{url: "file://EDSCTest_SimpleScanAndUnionWithMultipleSourcesAggRequestPipe1", storageType: "pipe", fileType: "bson"}]
    }, {
        collName: "coll2",
        dataSources: [{url: "file://EDSCTest_SimpleScanAndUnionWithMultipleSourcesAggRequestPipe2", storageType: "pipe", fileType: "bson"}]
    }]
}
    )");

    auto swAggReq = aggregation_request_helper::parseFromBSONForTests(kDatabaseName, aggCmdObj);
    ASSERT_OK(swAggReq.getStatus());
    auto swCursor = DBClientCursor::fromAggregationRequest(
        &client, swAggReq.getValue(), /*secondaryOk*/ false, /*useExhaust*/ false);
    ASSERT_OK(swCursor.getStatus());

    auto cursor = std::move(swCursor.getValue());
    int resCnt = 0;
    while (cursor->more()) {
        auto doc = cursor->next();
        // Simple scan from 'coll1' first and then $unionWith from 'coll2'.
        ASSERT_BSONOBJ_EQ(doc, srcDocs[resCnt % nDocs]);
        ++resCnt;
    }
    ASSERT_EQ(resCnt, nDocs * 2);

    // The second explain request. This verifies that virtual collections are cleaned up after
    // the aggregation request is done.
    verifyExplainAggCommand(client, aggCmdObj);
}

TEST_F(ExternalDataSourceCommandsTest, GroupAggRequest) {
    std::vector<BSONObj> srcDocs = {
        fromjson(R"(
            {
                "_id" : 1,
                "item" : "a",
                "quantity" : 2
            })"),
        fromjson(R"(
            {
                "_id" : 2,
                "item" : "b",
                "quantity" : 1
            })"),
        fromjson(R"(
            {
                "_id" : 3,
                "item" : "a",
                "quantity" : 5
            })"),
        fromjson(R"(
            {
                "_id" : 4,
                "item" : "b",
                "quantity" : 10
            })"),
        fromjson(R"(
            {
                "_id" : 5,
                "item" : "c",
                "quantity" : 10
            })"),
    };
    PipeWaiter pw;

    stdx::thread producer([&] {
        NamedPipeOutput pipeWriter("EDSCTest_GroupAggRequestPipe");
        pw.notify();
        pipeWriter.open();
        for (auto&& srcDoc : srcDocs) {
            pipeWriter.write(srcDoc.objdata(), srcDoc.objsize());
        }
        pipeWriter.close();
    });
    ON_BLOCK_EXIT([&] { producer.join(); });

    // Gives some time to the producer so that it can initialize a named pipe.
    pw.wait();

    DBDirectClient client(_opCtx);
    auto aggCmdObj = fromjson(R"(
{
    aggregate: "coll",
    pipeline: [{$group: {_id: "$item", o: {$sum: "$quantity"}}}],
    cursor: {},
    $_externalDataSources: [{
        collName: "coll",
        dataSources: [{url: "file://EDSCTest_GroupAggRequestPipe", storageType: "pipe", fileType: "bson"}]
    }]
}
    )");
    std::vector<BSONObj> expectedRes = {
        fromjson(R"(
            {
                "_id" : "a",
                "o" : 7
            })"),
        fromjson(R"(
            {
                "_id" : "b",
                "o" : 11
            })"),
        fromjson(R"(
            {
                "_id" : "c",
                "o" : 10
            })"),
    };

    auto swAggReq = aggregation_request_helper::parseFromBSONForTests(kDatabaseName, aggCmdObj);
    ASSERT_OK(swAggReq.getStatus());
    auto swCursor = DBClientCursor::fromAggregationRequest(
        &client, swAggReq.getValue(), /*secondaryOk*/ false, /*useExhaust*/ false);
    ASSERT_OK(swCursor.getStatus());

    auto cursor = std::move(swCursor.getValue());
    int resCnt = 0;
    while (cursor->more()) {
        auto doc = cursor->next();
        // Result set is pretty small and so we use linear search of vector.
        ASSERT_TRUE(
            std::find_if(expectedRes.begin(), expectedRes.end(), [&](const BSONObj& expectedObj) {
                return expectedObj.objsize() == doc.objsize() &&
                    std::memcmp(expectedObj.objdata(), doc.objdata(), expectedObj.objsize()) == 0;
            }) != expectedRes.end());
        ++resCnt;
    }
    ASSERT_EQ(resCnt, expectedRes.size());

    // The second explain request. This verifies that virtual collections are cleaned up after
    // the aggregation request is done.
    verifyExplainAggCommand(client, aggCmdObj);
}

TEST_F(ExternalDataSourceCommandsTest, LookupAggRequest) {
    std::vector<BSONObj> srcDocs = {
        fromjson(R"(
            {
                "a" : 1,
                "data" : "abcd"
            })"),
        fromjson(R"(
            {
                "a" : 2,
                "data" : "efgh"
            })"),
        fromjson(R"(
            {
                "a" : 3,
                "data" : "ijkl"
            })"),
    };
    PipeWaiter pw;

    // For the $lookup stage, we need data to be available for both named pipes simultaneously
    // because $lookup would read data from both collections and so we use two different named
    // pipes and pushes data into the inner side first. To avoid racy condition, notify the reader
    // side after both named pipes are created. This order is geared toward hash join algorithm.
    stdx::thread producer([&] {
        NamedPipeOutput pipeWriter2("EDSCTest_LookupAggRequestPipe2");
        NamedPipeOutput pipeWriter1("EDSCTest_LookupAggRequestPipe1");
        pw.notify();

        // Pushes data into the inner side (== coll2 with named_pipe2) first because the hash join
        // builds the inner (or build) side first.
        pipeWriter2.open();
        for (auto&& srcDoc : srcDocs) {
            pipeWriter2.write(srcDoc.objdata(), srcDoc.objsize());
        }
        pipeWriter2.close();

        pipeWriter1.open();
        for (auto&& srcDoc : srcDocs) {
            pipeWriter1.write(srcDoc.objdata(), srcDoc.objsize());
        }
        pipeWriter1.close();
    });
    ON_BLOCK_EXIT([&] { producer.join(); });

    // Gives some time to the producer so that it can initialize a named pipe.
    pw.wait();

    DBDirectClient client(_opCtx);
    auto aggCmdObj = fromjson(R"(
{
    aggregate: "coll1",
    pipeline: [{$lookup: {from: "coll2", localField: "a", foreignField: "a", as: "o"}}],
    cursor: {},
    $_externalDataSources: [{
        collName: "coll1",
        dataSources: [{url: "file://EDSCTest_LookupAggRequestPipe1", storageType: "pipe", fileType: "bson"}]
    }, {
        collName: "coll2",
        dataSources: [{url: "file://EDSCTest_LookupAggRequestPipe2", storageType: "pipe", fileType: "bson"}]
    }]
}
    )");
    std::vector<BSONObj> expectedRes = {
        fromjson(R"(
            {
                "a" : 1,
                "data" : "abcd",
                "o" : [{"a": 1, "data": "abcd"}]
            })"),
        fromjson(R"(
            {
                "a" : 2,
                "data" : "efgh",
                "o" : [{"a": 2, "data": "efgh"}]
            })"),
        fromjson(R"(
            {
                "a" : 3,
                "data" : "ijkl",
                "o" : [{"a": 3, "data": "ijkl"}]
            })"),
    };

    auto swAggReq = aggregation_request_helper::parseFromBSONForTests(kDatabaseName, aggCmdObj);
    ASSERT_OK(swAggReq.getStatus());
    auto swCursor = DBClientCursor::fromAggregationRequest(
        &client, swAggReq.getValue(), /*secondaryOk*/ false, /*useExhaust*/ false);
    ASSERT_OK(swCursor.getStatus());

    auto cursor = std::move(swCursor.getValue());
    int resCnt = 0;
    while (cursor->more()) {
        auto doc = cursor->next();
        // Result set is pretty small and so we use linear search of vector.
        ASSERT_TRUE(
            std::find_if(expectedRes.begin(), expectedRes.end(), [&](const BSONObj& expectedObj) {
                return expectedObj.objsize() == doc.objsize() &&
                    std::memcmp(expectedObj.objdata(), doc.objdata(), expectedObj.objsize()) == 0;
            }) != expectedRes.end());
        ++resCnt;
    }
    ASSERT_EQ(resCnt, expectedRes.size());

    // The second explain request. This verifies that virtual collections are cleaned up after
    // the aggregation request is done.
    verifyExplainAggCommand(client, aggCmdObj);
}
}  // namespace
}  // namespace mongo
