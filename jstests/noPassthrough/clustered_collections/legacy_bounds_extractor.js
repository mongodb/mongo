/**
 * Tests that clustered collscans use the legacy bound extractor with the feature flag off.
 */
import {getPlanStage, getWinningPlanFromExplain} from "jstests/libs/query/analyze_plan.js";

const conn = MongoRunner.runMongod({
    setParameter: {
        featureFlagClusteredCollScanMultiRange: false,
        internalQueryFrameworkControl: "forceClassicEngine",
    },
});

const testDB = conn.getDB("test");
const coll = testDB[jsTestName()];

coll.drop();
assert.commandWorked(
    testDB.createCollection(coll.getName(), {clusteredIndex: {unique: true, key: {_id: 1}}}),
);
assert.commandWorked(coll.insertMany([{_id: 0}, {_id: 1}, {_id: 2}]));

const filter = {_id: {$in: [0, 2]}};

function verifyResults() {
    const results = coll.find(filter).sort({_id: 1}).toArray();
    assert.eq(
        results.map((d) => d._id),
        [0, 2],
        {results},
    );
}

let explain = coll.explain("executionStats").find(filter).finish();
let winningPlan = getWinningPlanFromExplain(explain);

// With the flag off, the legacy path produces a CLUSTERED_IXSCAN for the entire [0, 2] interval.
// The filter must be present on the stage.
let collscan = getPlanStage(winningPlan, "CLUSTERED_IXSCAN");
assert(collscan, "Expected CLUSTERED_IXSCAN with flag off", winningPlan);
assert(collscan.filter, "Expected filter to be present (not simplified away)", winningPlan);

verifyResults();

// As a negative control, try it with the feature flag on.
assert.commandWorked(
    testDB.adminCommand({setParameter: 1, featureFlagClusteredCollScanMultiRange: true}),
);

explain = coll.explain("executionStats").find(filter).sort({_id: 1}).finish();
winningPlan = getWinningPlanFromExplain(explain);
collscan = getPlanStage(winningPlan, "CLUSTERED_IXSCAN");
assert(collscan, "Expected a clustered collscan with the flag on", winningPlan);
assert(!collscan.filter, "Expected no filter with the flag on", winningPlan);

// The results must be correct even without a filter.
verifyResults();

MongoRunner.stopMongod(conn);
