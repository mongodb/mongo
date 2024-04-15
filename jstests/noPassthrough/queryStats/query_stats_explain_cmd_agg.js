/**
 * This test confirms that the correct verbosity levels are stored in the query stats key for
 * explain commands on an agg query.
 * @tags: [requires_fcv_72]
 */
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {
    getLatestQueryStatsEntry,
    getValueAtPath,
    withQueryStatsEnabled,
} from "jstests/libs/query_stats_utils.js";

const testColl = jsTestName();

const testPipeline = [{"$match": {"bar": {"$gte": 1}}}, {"$count": "bars"}];

function assertVerbosityField(coll, pipeline, verbosity) {
    assert.commandWorked(coll.explain(verbosity).aggregate(pipeline));
    const entry = getLatestQueryStatsEntry(coll.getDB().getMongo(), {collName: coll.getName()});
    assert.eq(getValueAtPath(entry, "key.explain"), verbosity, tojson(entry));
    assert.eq(getValueAtPath(entry, "metrics.execCount"), 1, tojson(entry));
    // TODO SERVER-89439: Uncomment this block and perhaps add more assertions.
    // if (!FixtureHelpers.isSharded(coll)) {
    // TODO SERVER-89053: Remove conditional statement to assert in sharded tests.
    //     assert.gt(getValueAtPath(entry, "metrics.lastExecutionMicros"), 1, tojson(entry));
    // }
}

withQueryStatsEnabled(testColl, (coll) => {
    // Insert documents to ensure we raise 'execCount'.
    const bulk = coll.initializeUnorderedBulkOp();
    const numDocs = 100;
    for (let i = 0; i < numDocs / 2; ++i) {
        bulk.insert({foo: 0, bar: Math.floor(Math.random() * 3)});
        bulk.insert({foo: 1, bar: Math.floor(Math.random() * -2)});
    }
    assert.commandWorked(bulk.execute());

    assertVerbosityField(coll, testPipeline, 'queryPlanner');
    assertVerbosityField(coll, testPipeline, 'allPlansExecution');
    assertVerbosityField(coll, testPipeline, 'executionStats');
});
