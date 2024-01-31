/**
 * This test validates optimizer metrics in query stats supplemental metrics section.
 * @tags: [requires_fcv_71]
 */
import {getQueryStats} from "jstests/libs/query_stats_utils.js";

const conn = MongoRunner.runMongod({
    setParameter: {
        featureFlagCommonQueryFramework: true,
        internalQueryCollectOptimizerMetrics: true,
        internalQueryStatsRateLimit: -1
    }
});

assert.neq(null, conn, "mongod was unable to start up");

const dbName = jsTestName();
const db = conn.getDB(dbName);

function verifyOptimizerStats(statType, checkCostBasedEstimates) {
    const collName = statType;
    const coll = db[collName];
    coll.drop();

    for (let i = 0; i < 100; i++) {
        coll.insert({a: i, b: Math.random(0, 100)});
    }

    const cur = coll.find({}, {a: 1, b: 1});
    cur.next();

    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "tryBonsai"}));
    let stats = getQueryStats(conn, {collName: statType});
    assert.eq(1, stats.length);

    for (const entry of stats) {
        const entryMetrics = entry.metrics.supplementalMetrics;
        jsTestLog(tojson(entryMetrics));
        assert.eq(Object.entries(entryMetrics).length, 1);
        for (const [metricKey, metricVal] of Object.entries(entryMetrics)) {
            assert.eq(metricKey, statType);
            assert.eq(metricVal.updateCount, 1);
            assert.gt(metricVal.optimizationTimeMicros.min, 0);
            if (checkCostBasedEstimates) {
                assert.gt(metricVal.estimatedCost.min, 0);
                assert.gt(metricVal.estimatedCardinality.min, 0);
            }
        }
    }
}

assert.commandWorked(
    db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "forceClassicEngine"}));
verifyOptimizerStats("Classic", false);
assert.commandWorked(
    db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "trySbeEngine"}));
verifyOptimizerStats("SBE", false);
assert.commandWorked(
    db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "tryBonsai"}));
verifyOptimizerStats("BonsaiM2", true);
assert.commandWorked(
    db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "tryBonsaiExperimental"}));
verifyOptimizerStats("BonsaiM4", true);
assert.commandWorked(
    db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "forceBonsai"}));
verifyOptimizerStats("ForceBonsai", true);

MongoRunner.stopMongod(conn);
