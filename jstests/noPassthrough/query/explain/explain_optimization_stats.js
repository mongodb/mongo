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

    function collectOptimizationTimes(explain, fieldName) {
        if (explain === null || typeof explain !== "object") {
            return [];
        }
        if (Array.isArray(explain)) {
            return explain.flatMap((subExplain) => collectOptimizationTimes(subExplain, fieldName));
        } else {
            let ownResults = [];
            if (explain.hasOwnProperty(fieldName)) {
                ownResults = [explain[fieldName]];
            }
            return Object.keys(explain)
                .flatMap((key) => collectOptimizationTimes(explain[key], fieldName))
                .concat(ownResults);
        }
    }

    for (let testCase of testCases) {
        jsTest.log.info(`Test explain on ${testCase.testName} command`);

        runWithFailpoint(db, testCase.failpointName, testCase.failpointOpts, () => {
            const explain = assert.commandWorked(db.runCommand(testCase.command));
            // Assert the optimizationTimeMillis field is reported in explain as expected.
            const optimizationTimeMillis = collectOptimizationTimes(
                explain,
                "optimizationTimeMillis",
            );
            optimizationTimeMillis.forEach((time) => assert.gte(time, waitTimeMillis, explain));
            assert.gt(optimizationTimeMillis.length, 0, explain);

            const optimizationTimeMicros = collectOptimizationTimes(
                explain,
                "optimizationTimeMicros",
            );

            assert.gt(optimizationTimeMicros.length, 0, explain);
            optimizationTimeMicros.forEach((time) =>
                assert.gte(time, waitTimeMillis * 1000, explain),
            );

            // Regression test - ensure we never have optimizationTimeNanos.
            assert.eq(
                collectOptimizationTimes(explain, "optimizationTimeNanos").length,
                0,
                explain,
            );
        });
    }
}

jsTest.log.info("Testing standalone");
(function testStandalone() {
    const conn = MongoRunner.runMongod();
    const db = conn.getDB(jsTestName());
    try {
        runTest(db);
    } finally {
        MongoRunner.stopMongod(conn);
    }
})();

jsTest.log.info("Testing replica set");
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

jsTest.log.info("Testing sharded cluster");
(function testShardedCluster() {
    const st = new ShardingTest({shards: 2, config: 1});
    const db = st.s.getDB(jsTestName());
    try {
        runTest(db);
    } finally {
        st.stop();
    }
})();
