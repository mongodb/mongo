/**
 * Test the use of "explain" with the "$vectorSearch" aggregation stage.
 * @tags: [
 *  requires_fcv_71,
 * ]
 */
import {getAggPlanStage} from "jstests/libs/analyze_plan.js";
import {getUUIDFromListCollections} from "jstests/libs/uuid_util.js";
import {
    mongotCommandForVectorSearchQuery,
    MongotMock
} from "jstests/with_mongot/mongotmock/lib/mongotmock.js";

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
coll.insert({_id: 0});

const collectionUUID = getUUIDFromListCollections(testDB, collName);

const queryVector = [1.0, 2.0, 3.0];
const path = "x";
const numCandidates = NumberLong(10);
const limit = NumberLong(5);
const index = "index";
const filter = {
    x: {$gt: 0}
};

const explainContents = {
    profession: "writer"
};

const vectorSearchQuery = {
    queryVector,
    path,
    index,
    limit,
    numCandidates,
    filter,
};

const cursorId = NumberLong(123);

function testExplainVerbosity(verbosity) {
    const pipeline = [{$vectorSearch: vectorSearchQuery}];
    // Give mongotmock some stuff to return.
    {
        const history = [{
            expectedCommand: mongotCommandForVectorSearchQuery({
                ...vectorSearchQuery,
                explain: {verbosity},
                collName,
                dbName,
                collectionUUID,
            }),
            response: {explain: explainContents, ok: 1}
        }];
        assert.commandWorked(
            mongotConn.adminCommand({setMockResponses: 1, cursorId: cursorId, history: history}));
    }

    const result = coll.explain(verbosity).aggregate(pipeline);

    // Check for all appropriate stages and content of the stages.
    const searchStage = getAggPlanStage(result, "$vectorSearch");
    const lookupStage = getAggPlanStage(result, "$_internalSearchIdLookup");
    assert.neq(searchStage, null, searchStage);
    assert.neq(lookupStage, null, lookupStage);
    const stage = searchStage["$vectorSearch"];
    assert(stage.hasOwnProperty("explain"), stage);
    assert.eq(explainContents, stage["explain"]);

    const vectorSearchExplainQuery = Object.assign(vectorSearchQuery, {queryVector: "redacted"});
    assert.eq({...vectorSearchExplainQuery, explain: explainContents}, stage);
}

testExplainVerbosity("queryPlanner");
testExplainVerbosity("executionStats");
testExplainVerbosity("allPlansExecution");

MongoRunner.stopMongod(conn);
mongotmock.stop();
