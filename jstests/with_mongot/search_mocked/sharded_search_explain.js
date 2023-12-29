/**
 * Sharding tests for using "explain" with the $search aggregation stage.
 */
import {getAggPlanStages} from "jstests/libs/analyze_plan.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {checkSbeRestrictedOrFullyEnabled} from "jstests/libs/sbe_util.js";
import {getUUIDFromListCollections} from "jstests/libs/uuid_util.js";
import {
    ShardingTestWithMongotMock
} from "jstests/with_mongot/mongotmock/lib/shardingtest_with_mongotmock.js";

const dbName = "test";
const collName = "sharded_search_explain";

const stWithMock = new ShardingTestWithMongotMock({
    name: "sharded_search",
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
if (checkSbeRestrictedOrFullyEnabled(testDB) &&
    FeatureFlagUtil.isPresentAndEnabled(testDB.getMongo(), 'SearchInSbe')) {
    jsTestLog("Skipping the test because it only applies to $search in classic engine.");
    stWithMock.stop();
    quit();
}

assert.commandWorked(
    mongos.getDB("admin").runCommand({enableSharding: dbName, primaryShard: st.shard0.name}));

const coll = testDB.getCollection(collName);

assert.commandWorked(coll.insert({_id: 1, name: "Sokka"}));
assert.commandWorked(coll.insert({_id: 2, name: "Zuko", element: "fire"}));
assert.commandWorked(coll.insert({_id: 3, name: "Katara", element: "water"}));
assert.commandWorked(coll.insert({_id: 4, name: "Toph", element: "earth"}));
assert.commandWorked(coll.insert({_id: 11, name: "Aang", element: "air"}));
assert.commandWorked(coll.insert({_id: 12, name: "Ty Lee"}));
assert.commandWorked(coll.insert({_id: 13, name: "Azula", element: "fire"}));
assert.commandWorked(coll.insert({_id: 14, name: "Iroh", element: "fire"}));

// Shard the test collection, split it at {_id: 10}, and move the higher chunk to shard1.
st.shardColl(coll, {_id: 1}, {_id: 10}, {_id: 10 + 1});

const collUUID = getUUIDFromListCollections(st.rs0.getPrimary().getDB(dbName), collName);

const searchQuery = {
    query: "fire",
    path: "element"
};

const explainContents = {
    destiny: "avatar"
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

// Tests $search works with each explain verbosity.
for (const currentVerbosity of ["queryPlanner", "executionStats", "allPlansExecution"]) {
    function testExplainCase(shard0Conn, shard1Conn) {
        // Ensure there is never a staleShardVersionException to cause a retry on any shard.
        // If a retry happens on one shard and not another, then the shard that did not retry
        // will see multiple instances of the explain command, which the test does not expect,
        // causing an error.
        st.refreshCatalogCacheForNs(mongos, coll.getFullName());

        const searchCmd = {
            search: collName,
            collectionUUID: collUUID,
            query: searchQuery,
            explain: {verbosity: currentVerbosity},
            $db: dbName
        };

        const mergingPipelineHistory = [{
            expectedCommand: {
                planShardedSearch: collName,
                query: searchQuery,
                $db: dbName,
                searchFeatures: {shardedSort: 1}
            },
            response: {
                ok: 1,
                protocolVersion: NumberInt(42),
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
        stWithMock.getMockConnectedToHost(stWithMock.st.s)
            .setMockResponses(mergingPipelineHistory, cursorId);

        const history = [{
            expectedCommand: searchCmd,
            response: {explain: explainContents, ok: 1},
        }];
        // sX is shard num X.
        const s0Mongot = stWithMock.getMockConnectedToHost(shard0Conn);
        s0Mongot.setMockResponses(history, cursorId);

        const s1Mongot = stWithMock.getMockConnectedToHost(shard1Conn);
        s1Mongot.setMockResponses(history, cursorId);

        const result = coll.explain(currentVerbosity).aggregate([{$search: searchQuery}]);

        const searchStages = getAggPlanStages(result, "$_internalSearchMongotRemote");
        assert.eq(searchStages.length,
                  2,
                  searchStages);  // check there are 2 explain results for 2 shards
        for (const outerStage of searchStages) {
            const stage = outerStage["$_internalSearchMongotRemote"];
            assert(stage.hasOwnProperty("explain"), stage);
            assert.eq(explainContents, stage["explain"]);
        }
    }
    runTestOnPrimaries(testExplainCase);
    runTestOnSecondaries(testExplainCase);
}
stWithMock.stop();
