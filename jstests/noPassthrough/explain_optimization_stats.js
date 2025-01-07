/**
 * Tests for validating that optimization stats are included in explain output.
 */

const collName = "jstests_explain_optimization_stats";
import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";

function runTest(db) {
    assertDropAndRecreateCollection(db, collName);

    const collection = db[collName];

    assert.commandWorked(collection.createIndex({a: 1}));
    assert.commandWorked(collection.createIndex({b: 1}));
    assert.commandWorked(
        collection.insertMany(Array.from({length: 100}, (_, i) => ({a: "abc", b: "def", c: i}))));

    const filter = {a: "abc", b: "def", c: {$gt: 50}};
    const commands = [
        {testName: "find", command: {explain: {find: collName, filter: filter}}},
        {testName: "count", command: {explain: {count: collName, query: filter}}},
        {testName: "distinct", command: {explain: {distinct: collName, key: "c", query: filter}}},
        {
            testName: "findAndModify",
            command: {explain: {findAndModify: collName, query: filter, update: {$inc: {c: 1}}}}
        },
        {
            testName: "delete",
            command: {explain: {delete: collName, deletes: [{q: filter, limit: 0}]}}
        },
        {
            testName: "update",
            command: {explain: {update: collName, updates: [{q: filter, u: {$inc: {c: 1}}}]}}
        },
        {
            testName: "aggregate with explain command",
            command: {explain: {aggregate: collName, pipeline: [{$match: filter}], cursor: {}}}
        },
        {
            testName: "aggregate with explain flag",
            command: {aggregate: collName, pipeline: [{$match: filter}], cursor: {}, explain: true}
        },
        {
            testName: "mapReduce",
            command: {
                explain: {
                    mapReduce: collName,
                    query: filter,
                    map: function() {
                        emit("val", 1);
                    },
                    reduce: function(k, v) {
                        return 1;
                    },
                    out: "example"
                }
            }
        }
    ];

    function collectOptimizationTimeMillis(explain) {
        if (explain === null || typeof explain !== 'object') {
            return [];
        }

        if (Array.isArray(explain)) {
            return explain.flatMap(collectOptimizationTimeMillis);
        } else {
            let ownResults = [];
            if (explain.hasOwnProperty('optimizationTimeMillis')) {
                ownResults = [explain.optimizationTimeMillis];
            }
            return Object.keys(explain)
                .flatMap(key => collectOptimizationTimeMillis(explain[key]))
                .concat(ownResults);
        }
    }

    let failPoints = [];

    try {
        failPoints = FixtureHelpers.mapOnEachShardNode({
            db: db.getSiblingDB("admin"),
            func: (db) => configureFailPoint(db, "sleepWhileMultiplanning", {ms: 1000}),
            primaryNodeOnly: false,
        });

        for (let command of commands) {
            jsTestLog(`Test explain on ${command.testName} command`);
            const explain = assert.commandWorked(db.runCommand(command.command));
            const optimizationTimeMillis = collectOptimizationTimeMillis(explain);
            optimizationTimeMillis.forEach(time => assert.gte(time, 1000, explain));
            assert.gt(optimizationTimeMillis.length, 0, explain);
        }

    } finally {
        failPoints.forEach(failPoint => failPoint.off());
    }
}

jsTestLog("Testing standalone");
(function testStandalone() {
    const conn = MongoRunner.runMongod();
    const db = conn.getDB(jsTestName());
    try {
        runTest(db);
    } finally {
        MongoRunner.stopMongod(conn);
    }
})();

jsTestLog("Testing replica set");
(function testReplicaSet() {
    const rst = new ReplSetTest({nodes: 2});
    rst.startSet();
    rst.initiate();
    const db = rst.getPrimary().getDB(jsTestName());
    try {
        runTest(db);
    } finally {
        rst.stopSet();
    }
})();

jsTestLog("Testing on sharded cluster");
(function testShardedCluster() {
    const st = new ShardingTest({shards: 2, config: 1});
    const db = st.s.getDB(jsTestName());
    try {
        runTest(db);
    } finally {
        st.stop();
    }
})();
