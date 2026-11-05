/**
 * Asserts that query stats count the execution engine that ran each query.
 *
 * @tags: [
 *   requires_fcv_90,
 *   requires_sharding,
 * ]
 */
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {getEngine} from "jstests/libs/query/analyze_plan.js";
import {setParameterOnAllNonConfigNodes} from "jstests/noPassthrough/libs/server_parameter_helpers.js";
import {
    getQueryStats,
    resetQueryStatsStore,
    withQueryStatsEnabled,
} from "jstests/libs/query/query_stats_utils.js";

function runEngineCounterTest(coll) {
    const statsConn = coll.getDB().getMongo();
    const isSharded = FixtureHelpers.isMongos(coll.getDB());
    const shardCount = isSharded ? 2 : 1;

    assert.commandWorked(coll.insert([{a: 1}, {a: 2}, {a: 3}]));

    function getEngineCounters() {
        const stats = getQueryStats(statsConn, {collName: coll.getName()});
        assert.eq(1, stats.length, "expected exactly one query stats entry", {stats});
        const metrics = stats[0].metrics;
        const queryPlanner = metrics.hasOwnProperty("queryPlanner")
            ? metrics.queryPlanner
            : metrics;
        return queryPlanner.planShapeCounters.engines;
    }

    function runAndCount(pipeline, repetitions = 1) {
        resetQueryStatsStore(statsConn, "1MB");
        for (let i = 0; i < repetitions; i++) {
            coll.aggregate(pipeline).itcount();
        }
        return getEngineCounters();
    }

    function assertEngineCounters(pipeline, engine, repetitions = 1) {
        const explain = coll.explain().aggregate(pipeline);
        assert.eq(engine, getEngine(explain), explain);
        assert.eq(
            {[engine]: NumberLong(shardCount * repetitions)},
            runAndCount(pipeline, repetitions),
        );
    }

    function setFramework(value) {
        setParameterOnAllNonConfigNodes(statsConn, "internalQueryFrameworkControl", value);
    }

    const classicQuery = [{$match: {a: {$gte: 0}}}, {$sort: {a: 1}}];
    const sbeQuery = [{$group: {_id: "$a", n: {$sum: 1}}}];

    setFramework("forceClassicEngine");
    assertEngineCounters(classicQuery, "classic");
    assertEngineCounters(classicQuery, "classic", 3);

    setFramework("trySbeEngine");
    assertEngineCounters(sbeQuery, "sbe");
    assertEngineCounters(sbeQuery, "sbe", 3);
}

withQueryStatsEnabled(jsTestName(), runEngineCounterTest, {a: 1}, {a: 2});
