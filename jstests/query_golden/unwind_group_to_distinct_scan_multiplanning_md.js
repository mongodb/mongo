/**
 * Tests the interaction between multiplanning and the $unwind+$group to DISTINCT_SCAN rewrite.
 * The rewrite requires an empty filter, so there are no predicates to enumerate competing plans
 * from and the planner usually generates a single solution.
 *
 * @tags: [
 *   featureFlagShardFilteringDistinctScan,
 *   requires_fcv_91
 * ]
 */
import {section} from "jstests/libs/query/pretty_md.js";
import {outputAggregationPlanAndResults} from "jstests/libs/query/golden_test_utils.js";

const coll = db[jsTestName()];
coll.drop();
coll.insertMany([{a: [1, 2], b: 1}, {a: [2, 3], b: 2}, {a: 7, b: 3}, {a: [], b: 4}, {b: 5}]);
coll.createIndex({a: 1});
coll.createIndex({a: 1, b: 1});

const pipeline = [{$unwind: {path: "$a", preserveNullAndEmptyArrays: true}}, {$group: {_id: "$a"}}];

section("Multiple suitable indexes generate a single DISTINCT_SCAN candidate");
outputAggregationPlanAndResults(coll, pipeline);

section("A hinted index that requires a fetch is not converted to a DISTINCT_SCAN");
outputAggregationPlanAndResults(coll, pipeline, {hint: {a: 1, b: 1}});

section("A hint can force the suitable index");
outputAggregationPlanAndResults(coll, pipeline, {hint: {a: 1}});

// $match is not supported yet, so we need to use
// 'internalQueryPlannerGenerateCoveredWholeIndexScans' to force a multiplanning scenario.
// 'runWithKnobs()' and similar helpers go through 'FixtureHelpers', which opens new connections
// whose implicit sessions print their ids into the golden output, so we set the knob directly.
section("A whole index scan candidate multiplans against the DISTINCT_SCAN");
const scalarColl = db[jsTestName() + "_scalar"];
scalarColl.drop();
scalarColl.insertMany([{a: 1, b: 1}, {a: 1, b: 2}, {a: 2, b: 3}, {a: 3, b: 4}, {b: 5}]);
scalarColl.createIndex({b: 1, a: 1});
scalarColl.createIndex({a: 1});
const knob = "internalQueryPlannerGenerateCoveredWholeIndexScans";
const priorKnobValue = assert.commandWorked(db.adminCommand({getParameter: 1, [knob]: 1}))[knob];
assert.commandWorked(db.adminCommand({setParameter: 1, [knob]: true}));
try {
    outputAggregationPlanAndResults(scalarColl, pipeline);
} finally {
    assert.commandWorked(db.adminCommand({setParameter: 1, [knob]: priorKnobValue}));
}
