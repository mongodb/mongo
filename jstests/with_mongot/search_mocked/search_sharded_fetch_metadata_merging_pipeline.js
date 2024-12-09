/**
 * Test that we properly fetch the metadata merging pipeline during planning.
 */
import {getUUIDFromListCollections} from "jstests/libs/uuid_util.js";
import {mongotCommandForQuery} from "jstests/with_mongot/mongotmock/lib/mongotmock.js";
import {
    ShardingTestWithMongotMock
} from "jstests/with_mongot/mongotmock/lib/shardingtest_with_mongotmock.js";
import {
    searchShardedExampleCursors1,
    searchShardedExampleCursors2
} from "jstests/with_mongot/search_mocked/lib/search_sharded_example_cursors.js";

let nodeOptions = {setParameter: {enableTestCommands: 1}};

const stWithMock = new ShardingTestWithMongotMock({
    name: "sharded_search",
    shards: {
        rs0: {nodes: 1},
        rs1: {nodes: 1},
    },
    mongos: 1,
    other: {
        rsOptions: nodeOptions,
        mongosOptions: nodeOptions,
    }
});
stWithMock.start();
const st = stWithMock.st;

const conn = stWithMock.st.s;
const mongotConn = stWithMock.getMockConnectedToHost(conn);

const dbName = jsTestName();
const testDB = conn.getDB(dbName);
assert.commandWorked(
    conn.getDB("admin").runCommand({enableSharding: dbName, primaryShard: st.shard0.name}));

const collName = "meta";
const coll = testDB.getCollection(collName);
const collNS = coll.getFullName();

assert.commandWorked(coll.insert({"_id": 1, "title": "cakes"}));
assert.commandWorked(coll.insert({"_id": 2, "title": "cookies and cakes"}));
assert.commandWorked(coll.insert({"_id": 3, "title": "vegetables"}));
assert.commandWorked(coll.insert({"_id": 4, "title": "take and bake cakes"}));

st.shardColl(coll, {_id: 1}, {_id: 2}, {_id: 3}, dbName);

const currentVerbosity = "queryPlanner";

const searchQuery = {
    query: "cakes",
    path: "title"
};
const protocolVersion = NumberInt(42);

const mergingPipelineHistory = [{
    expectedCommand: {
        planShardedSearch: coll.getName(),
        query: searchQuery,
        $db: dbName,
        searchFeatures: {shardedSort: 1},
    },
    response: {
        ok: 1,
        protocolVersion: protocolVersion,
        metaPipeline: [{
            "$group": {
                "_id": {"type": "$type", "path": "$path", "bucket": "$bucket"},
                "value": {
                    "$sum": "$metaVal",
                }
            }
        }]
    }
}];

const explainMergingPipelineHistory = [{
    expectedCommand: {
        planShardedSearch: coll.getName(),
        query: searchQuery,
        $db: dbName,
        searchFeatures: {shardedSort: 1},
        explain: {verbosity: currentVerbosity}
    },
    response: {
        ok: 1,
        protocolVersion: protocolVersion,
        metaPipeline: [{
            "$group": {
                "_id": {"type": "$type", "path": "$path", "bucket": "$bucket"},
                "value": {
                    "$sum": "$metaVal",
                }
            }
        }]
    }
}];

const setPipelineOptimizationMode = (mode) => {
    testDB.adminCommand({configureFailPoint: 'disablePipelineOptimization', mode});
};

// Test that explain includes a $setVariableFromSubPipeline stage with the appropriate merging
// pipeline. Note that mongotmock state must be handled correctly here not to influence the next
// test(s).
function testExplain({shouldReferenceSearchMeta, disablePipelineOptimization}) {
    try {
        if (disablePipelineOptimization) {
            setPipelineOptimizationMode('alwaysOn');
        }
        mongotConn.setMockResponses(explainMergingPipelineHistory, 1);

        const mongotQuery = searchQuery;
        const collUUID0 = getUUIDFromListCollections(st.rs0.getPrimary().getDB(dbName), collName);
        const collUUID1 = getUUIDFromListCollections(st.rs1.getPrimary().getDB(dbName), collName);
        // History for shard 1.
        {
            const exampleCursor =
                searchShardedExampleCursors1(dbName,
                                             collNS,
                                             collName,
                                             // Explain doesn't take protocol version.
                                             mongotCommandForQuery({
                                                 query: mongotQuery,
                                                 collName: collName,
                                                 db: dbName,
                                                 collectionUUID: collUUID0,
                                                 explainVerbosity: {verbosity: currentVerbosity}
                                             }));
            exampleCursor.historyResults[0].response.explain = {destiny: "avatar"};
            const mongot = stWithMock.getMockConnectedToHost(st.rs0.getPrimary());
            mongot.setMockResponses(exampleCursor.historyResults, exampleCursor.resultsID);
            mongot.setMockResponses(exampleCursor.historyMeta, exampleCursor.metaID);
        }

        // History for shard 2
        {
            const exampleCursor =
                searchShardedExampleCursors2(dbName,
                                             collNS,
                                             collName,
                                             // Explain doesn't take protocol version.
                                             mongotCommandForQuery({
                                                 query: mongotQuery,
                                                 collName: collName,
                                                 db: dbName,
                                                 collectionUUID: collUUID1,
                                                 explainVerbosity: {verbosity: currentVerbosity}
                                             }));
            exampleCursor.historyResults[0].response.explain = {destiny: "avatar"};
            const mongot = stWithMock.getMockConnectedToHost(st.rs1.getPrimary());
            mongot.setMockResponses(exampleCursor.historyResults, exampleCursor.resultsID);
            mongot.setMockResponses(exampleCursor.historyMeta, exampleCursor.metaID);
        }

        let pipeline = shouldReferenceSearchMeta
            ? [{$search: searchQuery}, {$project: {"var": "$$SEARCH_META"}}]
            : [{$search: searchQuery}];
        let explain = coll.explain(currentVerbosity).aggregate(pipeline);
        let mergingPipeline = explain.splitPipeline.mergerPart;
        // First element in merging pipeline must be a $mergeCursors stage.
        assert.eq(["$mergeCursors"], Object.keys(mergingPipeline[0]));
        if (shouldReferenceSearchMeta || disablePipelineOptimization) {
            // Second element sets the variable given the sub-pipeline provided above.
            assert.eq({
                "$setVariableFromSubPipeline": {
                    "setVariable": "$$SEARCH_META",
                    "pipeline": [{
                        "$group": {
                            "_id": {"type": "$type", "path": "$path", "bucket": "$bucket"},
                            "value": {"$sum": "$metaVal"}
                        }
                    }]
                }
            },
                      mergingPipeline[1],
                      tojson(explain));
        } else {
            // No references to $$SEARCH_META in the pipeline should make us skip adding the
            // $setVariableFromSubPipeline stage.
            assert.eq(mergingPipeline.length, 1, tojson(explain));
        }
    } finally {
        if (disablePipelineOptimization) {
            // Reset the pipeline optimization failpoint if we set it at the start.
            setPipelineOptimizationMode('off');
        }
    }
}

// Set the mock responses for a query which includes the result cursors.
function setQueryMockResponses(removeGetMore) {
    mongotConn.setMockResponses(mergingPipelineHistory, 1);

    const mongotQuery = searchQuery;
    const collUUID0 = getUUIDFromListCollections(st.rs0.getPrimary().getDB(dbName), collName);
    const collUUID1 = getUUIDFromListCollections(st.rs1.getPrimary().getDB(dbName), collName);
    // History for shard 1.
    {
        const exampleCursor =
            searchShardedExampleCursors1(dbName, collNS, collName, mongotCommandForQuery({
                                             query: mongotQuery,
                                             collName: collName,
                                             db: dbName,
                                             collectionUUID: collUUID0,
                                             protocolVersion: protocolVersion
                                         }));
        // If the query we are setting responses for has a limit, the getMore is not needed.
        if (removeGetMore) {
            exampleCursor.historyResults.pop();
            exampleCursor.historyResults[0].response.cursors[0].cursor.id = NumberLong(0);
        }
        const mongot = stWithMock.getMockConnectedToHost(st.rs0.getPrimary());
        mongot.setMockResponses(exampleCursor.historyResults, exampleCursor.resultsID);
        mongot.setMockResponses(exampleCursor.historyMeta, exampleCursor.metaID);
    }

    // History for shard 2
    {
        const exampleCursor =
            searchShardedExampleCursors2(dbName, collNS, collName, mongotCommandForQuery({
                                             query: mongotQuery,
                                             collName: collName,
                                             db: dbName,
                                             collectionUUID: collUUID1,
                                             protocolVersion: protocolVersion
                                         }));
        if (removeGetMore) {
            exampleCursor.historyResults.pop();
            exampleCursor.historyResults[0].response.cursors[0].cursor.id = NumberLong(0);
        }
        const mongot = stWithMock.getMockConnectedToHost(st.rs1.getPrimary());
        mongot.setMockResponses(exampleCursor.historyResults, exampleCursor.resultsID);
        mongot.setMockResponses(exampleCursor.historyMeta, exampleCursor.metaID);
    }
}

// Test that a $search query properly computes the $$SEARCH_META value according to the pipeline
// returned by mongot(mock).
function testSearchQuery() {
    setQueryMockResponses(false);
    let queryResult =
        coll.aggregate([{$search: searchQuery}, {$project: {"var": "$$SEARCH_META"}}]);
    assert.eq([{_id: 1, var : {_id: {}, value: 56}}], queryResult.toArray());
}

// Test that a $searchMeta query properly computes the metadata value according to the pipeline
// returned by mongot(mock).
function testSearchMetaQuery() {
    setQueryMockResponses(true);
    let queryResult = coll.aggregate([{$searchMeta: searchQuery}]);
    // Same as above query result but not embedded in a document.
    assert.eq([{_id: {}, value: 56}], queryResult.toArray());
}

testExplain({shouldReferenceSearchMeta: true, disablePipelineOptimization: true});
testExplain({shouldReferenceSearchMeta: false, disablePipelineOptimization: true});

testExplain({shouldReferenceSearchMeta: true, disablePipelineOptimization: false});
testExplain({shouldReferenceSearchMeta: false, disablePipelineOptimization: false});

testSearchQuery();
testSearchMetaQuery();

stWithMock.stop();
