/**
 * Tests the interaction between multiplanning and the $unwind+$group to DISTINCT_SCAN rewrite.
 * The rewrite requires an empty filter, so there are no predicates to enumerate competing plans
 * from and the planner usually generates a single solution.
 *
 * @tags: [
 *   featureFlagShardFilteringDistinctScan,
 *   requires_fcv_90
 * ]
 */
import {section} from "jstests/libs/query/pretty_md.js";
import {outputAggregationPlanAndResults} from "jstests/libs/query/golden_test_utils.js";
import {runWithKnobs} from "jstests/libs/property_test_helpers/common_properties.js";

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
section("A whole index scan candidate multiplans against the DISTINCT_SCAN");
const scalarColl = db[jsTestName() + "_scalar"];
scalarColl.drop();
scalarColl.insertMany([{a: 1, b: 1}, {a: 1, b: 2}, {a: 2, b: 3}, {a: 3, b: 4}, {b: 5}]);
scalarColl.createIndex({b: 1, a: 1});
scalarColl.createIndex({a: 1});
runWithKnobs(db, () => outputAggregationPlanAndResults(scalarColl, pipeline), {
    internalQueryPlannerGenerateCoveredWholeIndexScans: true,
});
