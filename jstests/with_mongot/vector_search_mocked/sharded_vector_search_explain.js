/**
 * Sharding tests for using "explain" with the $vectorSearch aggregation stage.
 * @tags: [
 *  featureFlagVectorSearchPublicPreview,
 *  featureFlagSearchExplainExecutionStats_incompatible
 * ]
 */
import {getAggPlanStages} from "jstests/libs/analyze_plan.js";
import {getUUIDFromListCollections} from "jstests/libs/uuid_util.js";
import {mongotCommandForVectorSearchQuery} from "jstests/with_mongot/mongotmock/lib/mongotmock.js";
import {
    ShardingTestWithMongotMock
} from "jstests/with_mongot/mongotmock/lib/shardingtest_with_mongotmock.js";
import {prepCollection} from "jstests/with_mongot/mongotmock/lib/utils.js";

const dbName = jsTestName();
const collName = jsTestName();

const stWithMock = new ShardingTestWithMongotMock({
    name: jsTestName(),
    shards: {
        rs0: {nodes: 2},
        rs1: {nodes: 2},
    },
    mongos: 1,
});
stWithMock.start();
const st = stWithMock.st;

const mongos = st.s;
const testDB = mongos.getDB(dbName);
assert.commandWorked(
    mongos.getDB("admin").runCommand({enableSharding: dbName, primaryShard: st.shard0.name}));

const coll = testDB.getCollection(collName);
prepCollection(mongos, dbName, collName);

// Shard the test collection, split it at {_id: 10}, and move the higher chunk to shard1.
st.shardColl(coll, {_id: 1}, {_id: 10}, {_id: 10 + 1});

const collectionUUID = getUUIDFromListCollections(st.rs0.getPrimary().getDB(dbName), collName);

const vectorSearchQuery = {
    queryVector: [1.0, 2.0, 3.0],
    path: "x",
    numCandidates: 100,
    limit: 10,
    index: "idx",
    filter: {x: {$gt: 1}},
};

const lowerUserLimit = vectorSearchQuery.limit - 1;
const higherUserLimit = vectorSearchQuery.limit + 1;

const explainContents = {
    str: "this is a string",
    obj: {x: 1, y: 2},
};

const cursorId = NumberLong(123);

function runTestOnPrimaries(testFn) {
    testDB.getMongo().setReadPref("primary");
    testFn(st.rs0.getPrimary(), st.rs1.getPrimary());
}

function runTestOnSecondaries(testFn) {
    testDB.getMongo().setReadPref("secondary");
    testFn(st.rs0.getSecondary(), st.rs1.getSecondary());
}

function testExplainVerbosity(shard0Conn, shard1Conn, verbosity, userLimit) {
    // Ensure there is never a staleShardVersionException to cause a retry on any shard.
    // If a retry happens on one shard and not another, then the shard that did not retry
    // will see multiple instances of the explain command, which the test does not expect,
    // causing an error.
    st.refreshCatalogCacheForNs(mongos, coll.getFullName());

    const vectorSearchCmd = mongotCommandForVectorSearchQuery({
        ...vectorSearchQuery,
        explain: {verbosity},
        collName,
        dbName,
        collectionUUID,
    });

    const history = [{
        expectedCommand: vectorSearchCmd,
        response: {explain: explainContents, ok: 1},
    }];

    const s0Mongot = stWithMock.getMockConnectedToHost(shard0Conn);
    s0Mongot.setMockResponses(history, cursorId);

    const s1Mongot = stWithMock.getMockConnectedToHost(shard1Conn);
    s1Mongot.setMockResponses(history, cursorId);

    let pipeline = [{$vectorSearch: vectorSearchQuery}];

    if (userLimit) {
        pipeline.push({$limit: userLimit});
    }

    const result = coll.explain(verbosity).aggregate(pipeline);

    // We should have a $limit on each shard.
    const limitStages = getAggPlanStages(result, "$limit");
    assert.eq(limitStages.length, 2, tojson(result));
    // The $limits will take on the value of the $vectorSearch limit unless we have a smaller
    // user-specified $limit.
    const expectedLimitVal =
        userLimit ? Math.min(userLimit, vectorSearchQuery.limit) : vectorSearchQuery.limit;
    for (const limitStage of limitStages) {
        assert.eq(expectedLimitVal, limitStage["$limit"], tojson(result));
    }

    // The merging pipeline should also have a $limit with the minimum of the $vectorSearch limit
    // and user-specified $limit.
    assert(result.splitPipeline.mergerPart[0].hasOwnProperty("$mergeCursors"),
           tojson(result.splitPipeline));
    assert(result.splitPipeline.mergerPart[1].hasOwnProperty("$limit"),
           tojson(result.splitPipeline));
    assert.eq(
        result.splitPipeline.mergerPart[1].$limit, expectedLimitVal, tojson(result.splitPipeline));

    // Each shard should have an $_internalSearchIdLookup stage.
    const idLookupStages = getAggPlanStages(result, "$_internalSearchIdLookup");
    assert.eq(idLookupStages.length, 2, tojson(idLookupStages));

    // Each shard should have a $vectorSearch stage with explain info populated.
    const vectorSearchStages = getAggPlanStages(result, "$vectorSearch");
    assert.eq(vectorSearchStages.length, 2, tojson(vectorSearchStages));
    for (const vectorSearchStage of vectorSearchStages) {
        const vectorSearchInfo = vectorSearchStage["$vectorSearch"];
        assert(vectorSearchInfo.hasOwnProperty("explain"), tojson(vectorSearchInfo));
        assert.eq(explainContents, vectorSearchInfo.explain);
        assert.eq(
            vectorSearchQuery.queryVector, vectorSearchInfo.queryVector, tojson(vectorSearchInfo));
        assert.eq(vectorSearchQuery.path, vectorSearchInfo.path, tojson(vectorSearchInfo));
        assert.eq(vectorSearchQuery.numCandidates,
                  vectorSearchInfo.numCandidates,
                  tojson(vectorSearchInfo));
        assert.eq(vectorSearchQuery.limit, vectorSearchInfo.limit, tojson(vectorSearchInfo));
        assert.eq(vectorSearchQuery.index, vectorSearchInfo.index, tojson(vectorSearchInfo));
        assert.eq(vectorSearchQuery.filter, vectorSearchInfo.filter, tojson(vectorSearchInfo));
    }
}

// Test that $vectorSearch works with each explain verbosity.
runTestOnPrimaries((shard0Conn, shard1Conn) =>
                       testExplainVerbosity(shard0Conn, shard1Conn, "queryPlanner"));
runTestOnPrimaries((shard0Conn, shard1Conn) => testExplainVerbosity(
                       shard0Conn, shard1Conn, "queryPlanner", lowerUserLimit));
runTestOnPrimaries((shard0Conn, shard1Conn) => testExplainVerbosity(
                       shard0Conn, shard1Conn, "queryPlanner", higherUserLimit));
runTestOnSecondaries((shard0Conn, shard1Conn) =>
                         testExplainVerbosity(shard0Conn, shard1Conn, "queryPlanner"));

runTestOnPrimaries((shard0Conn, shard1Conn) =>
                       testExplainVerbosity(shard0Conn, shard1Conn, "executionStats"));
runTestOnPrimaries((shard0Conn, shard1Conn) => testExplainVerbosity(
                       shard0Conn, shard1Conn, "executionStats", lowerUserLimit));
runTestOnPrimaries((shard0Conn, shard1Conn) => testExplainVerbosity(
                       shard0Conn, shard1Conn, "executionStats", higherUserLimit));
runTestOnSecondaries((shard0Conn, shard1Conn) =>
                         testExplainVerbosity(shard0Conn, shard1Conn, "executionStats"));

runTestOnPrimaries((shard0Conn, shard1Conn) =>
                       testExplainVerbosity(shard0Conn, shard1Conn, "allPlansExecution"));
runTestOnPrimaries((shard0Conn, shard1Conn) => testExplainVerbosity(
                       shard0Conn, shard1Conn, "allPlansExecution", lowerUserLimit));
runTestOnPrimaries((shard0Conn, shard1Conn) => testExplainVerbosity(
                       shard0Conn, shard1Conn, "allPlansExecution", higherUserLimit));
runTestOnSecondaries((shard0Conn, shard1Conn) =>
                         testExplainVerbosity(shard0Conn, shard1Conn, "allPlansExecution"));

stWithMock.stop();
