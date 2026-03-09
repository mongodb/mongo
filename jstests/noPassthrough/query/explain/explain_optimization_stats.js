/**
 * Tests for validating that optimization stats are included in explain output.
 * @tags: [
 *   requires_scripting
 * ]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {
    runWithFailpoint,
    setupCollectionAndGetExplainTestCases,
} from "jstests/noPassthrough/query/explain/explain_and_profile_optimization_stats_util.js";

const collName = "jstests_explain_optimization_stats";
function runTest(db) {
    const waitTimeMillis = 500;
    const testCases = setupCollectionAndGetExplainTestCases(db, collName, waitTimeMillis);

    // Recursively search for any optimization time fields.  Because explain now
    // reports the value in millis, micros and nanos we return an array of objects
    //  containing both the value and its unit so the caller can compare it appropriately.
    function collectOptimizationTimes(explain) {
        if (explain === null || typeof explain !== "object") {
            return [];
        }

        if (Array.isArray(explain)) {
            return explain.flatMap(collectOptimizationTimes);
        } else {
            let ownResults = [];
            if (explain.hasOwnProperty("optimizationTimeMillis")) {
                ownResults.push({time: explain.optimizationTimeMillis, unit: "millis"});
            }
            if (explain.hasOwnProperty("optimizationTimeMicros")) {
                ownResults.push({time: explain.optimizationTimeMicros, unit: "micros"});
            }
            if (explain.hasOwnProperty("optimizationTimeNanos")) {
                ownResults.push({time: explain.optimizationTimeNanos, unit: "nanos"});
            }
            return Object.keys(explain)
                .flatMap((key) => collectOptimizationTimes(explain[key]))
                .concat(ownResults);
        }
    }

    for (let testCase of testCases) {
        jsTestLog(`Test explain on ${testCase.testName} command`);

        runWithFailpoint(db, testCase.failpointName, testCase.failpointOpts, () => {
            const explain = assert.commandWorked(db.runCommand(testCase.command));

            // Assert that at least one optimization time field is reported and that it
            // reflects the injected delay.  Convert the threshold based on the unit.
            const optimizationTimes = collectOptimizationTimes(explain);
            optimizationTimes.forEach(({time, unit}) => {
                if (unit === "micros") {
                    assert.gte(time, waitTimeMillis * 1000, explain);
                } else if (unit === "nanos") {
                    assert.gte(time, waitTimeMillis * 1000000, explain);
                } else {
                    assert.gte(time, waitTimeMillis, explain);
                }
            });
            assert.gt(optimizationTimes.length, 0, explain);
        });
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
