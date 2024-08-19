/**
 * Test the use of "explain" with the "$vectorSearch" aggregation stage.
 * @tags: [
 *  requires_fcv_71,
 * ]
 */

import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {getUUIDFromListCollections} from "jstests/libs/uuid_util.js";
import {
    mongotCommandForVectorSearchQuery,
    MongotMock
} from "jstests/with_mongot/mongotmock/lib/mongotmock.js";
import {
    getDefaultLastExplainContents,
    getSearchStagesAndVerifyExplainOutput,
    setUpMongotReturnExplain,
    setUpMongotReturnExplainAndCursor,
    setUpMongotReturnExplainAndCursorGetMore,
} from "jstests/with_mongot/mongotmock/lib/utils.js";

// Set up mongotmock and point the mongod to it.
const mongotmock = new MongotMock();
mongotmock.start();
const mongotConn = mongotmock.getConnection();

const conn = MongoRunner.runMongod({setParameter: {mongotHost: mongotConn.host}});
const dbName = jsTestName();
const collName = jsTestName();
const testDB = conn.getDB(dbName);
const coll = testDB.getCollection(collName);
coll.drop();
coll.insert({_id: 3});
coll.insert({_id: 2});
coll.insert({_id: 8});
coll.insert({_id: 20});

const collectionUUID = getUUIDFromListCollections(testDB, collName);

const queryVector = [1.0, 2.0, 3.0];
const path = "x";
const numCandidates = NumberLong(10);
const limit = NumberLong(5);
const index = "index";
const filter = {
    x: {$gt: 0}
};

const expectedExplainObject = getDefaultLastExplainContents();

const vectorSearchQuery = {
    queryVector,
    path,
    index,
    limit,
    numCandidates,
    filter,
};

const pipeline = [{$vectorSearch: vectorSearchQuery}];

function runExplainQueryPlannerTest() {
    const verbosity = "queryPlanner";
    const vectorSearchCmd = mongotCommandForVectorSearchQuery({
        ...vectorSearchQuery,
        explain: {verbosity},
        collName,
        dbName,
        collectionUUID,
    });
    setUpMongotReturnExplain({
        searchCmd: vectorSearchCmd,
        mongotMock: mongotmock,
    });
    const result = coll.explain(verbosity).aggregate(pipeline);

    getSearchStagesAndVerifyExplainOutput({
        result: result,
        stageType: "$vectorSearch",
        verbosity: verbosity,
        explainObject: expectedExplainObject,
    });
    getSearchStagesAndVerifyExplainOutput({
        result: result,
        stageType: "$_internalSearchIdLookup",
        verbosity: verbosity,
    });
}

function runExplainExecutionStatsTest(verbosity) {
    const vectorSearchCmd = mongotCommandForVectorSearchQuery({
        ...vectorSearchQuery,
        explain: {verbosity},
        collName,
        dbName,
        collectionUUID,
    });
    // TODO SERVER-91594: Test for setUpMongotReturnExplain() can be removed when mongot always
    // returns a cursor.
    {
        setUpMongotReturnExplain({
            searchCmd: vectorSearchCmd,
            mongotMock: mongotmock,
        });
        // When querying an older version of mongot for explain, the query is sent twice.
        // This uses a different cursorId than the default one for setUpMongotReturnExplain() so
        // the mock will return the response correctly.
        setUpMongotReturnExplain({
            searchCmd: vectorSearchCmd,
            mongotMock: mongotmock,
            cursorId: NumberLong(124),
        });
        const result = coll.explain(verbosity).aggregate(pipeline);
        getSearchStagesAndVerifyExplainOutput({
            result: result,
            stageType: "$vectorSearch",
            verbosity: verbosity,
            nReturned: NumberLong(0),
            explainObject: expectedExplainObject,
        });
        getSearchStagesAndVerifyExplainOutput({
            result,
            stageType: "$_internalSearchIdLookup",
            verbosity: verbosity,
            nReturned: NumberLong(0),
        });
    }
    {
        setUpMongotReturnExplainAndCursor({
            mongotMock: mongotmock,
            coll,
            searchCmd: vectorSearchCmd,
            nextBatch: [
                {_id: 3, $vectorSearchScore: 100},
                {_id: 2, $vectorSearchScore: 10},
                {_id: 4, $vectorSearchScore: 1},
                {_id: 8, $vectorSearchScore: 0.2},
            ],
        });
        const result = coll.explain(verbosity).aggregate(pipeline);
        getSearchStagesAndVerifyExplainOutput({
            result: result,
            stageType: "$vectorSearch",
            verbosity: verbosity,
            nReturned: NumberLong(4),
            explainObject: expectedExplainObject,
        });
        getSearchStagesAndVerifyExplainOutput({
            result: result,
            stageType: "$_internalSearchIdLookup",
            verbosity: verbosity,
            nReturned: NumberLong(3),
        });
    }
    {
        setUpMongotReturnExplainAndCursorGetMore({
            mongotMock: mongotmock,
            coll,
            searchCmd: vectorSearchCmd,
            batchList: [
                [{_id: 3, $vectorSearchScore: 100}, {_id: 2, $vectorSearchScore: 10}],
                [{_id: 4, $vectorSearchScore: 1}, {_id: 1, $vectorSearchScore: 0.99}],
                [{_id: 8, $vectorSearchScore: 0.2}]
            ],
        });
        const result = coll.explain(verbosity).aggregate(pipeline, {cursor: {batchSize: 2}});
        getSearchStagesAndVerifyExplainOutput({
            result: result,
            stageType: "$vectorSearch",
            verbosity: verbosity,
            nReturned: NumberLong(5),
            explainObject: expectedExplainObject,
        });
        getSearchStagesAndVerifyExplainOutput({
            result: result,
            stageType: "$_internalSearchIdLookup",
            verbosity: verbosity,
            nReturned: NumberLong(3),
        });
    }
}

runExplainQueryPlannerTest();
if (FeatureFlagUtil.isEnabled(testDB.getMongo(), 'SearchExplainExecutionStats')) {
    runExplainExecutionStatsTest("executionStats");
    runExplainExecutionStatsTest("allPlansExecution");
}

MongoRunner.stopMongod(conn);
mongotmock.stop();
