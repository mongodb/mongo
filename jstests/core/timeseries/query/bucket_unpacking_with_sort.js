/**
 * Test that the bucket unpacking with sorting rewrite is performed and doesn't cause incorrect
 * results to be created.
 *
 * @tags: [
 *     # Explain of a resolved view must be executed by mongos.
 *     directly_against_shardsvrs_incompatible,
 *     # This complicates aggregation extraction.
 *     do_not_wrap_aggregations_in_facets,
 *     # Refusing to run a test that issues an aggregation command with explain because it may
 *     # return incomplete results if interrupted by a stepdown.
 *     does_not_support_stepdowns,
 *     # We need a timeseries collection.
 *     requires_timeseries,
 *     # TODO (SERVER-88539) the timeseries setup runs a migration. Remove the upgrade-downgrade
 *     # incompatible tag once migrations  work during downgrade.
 *     cannot_run_during_upgrade_downgrade,
 *     requires_getmore,
 * ]
 */
import {
    backwardCollscan,
    backwardIxscan,
    forwardCollscan,
    forwardIxscan,
    runRewritesTest,
    setupColl
} from "jstests/core/timeseries/libs/timeseries_sort_util.js";

const collName = jsTestName();
const coll = db[collName];
const metaCollName = jsTestName() + '_with_meta';
const metaColl = db[metaCollName];
const metaCollSubFieldsName = jsTestName() + '_with_meta_sub';
const metaCollSubFields = db[metaCollSubFieldsName];
const subFields = ["a", "b"];

setupColl(coll, collName, false);
setupColl(metaColl, metaCollName, true);
setupColl(metaCollSubFields, metaCollSubFieldsName, true, subFields);

// Collscan cases
runRewritesTest({t: 1}, null, null, forwardCollscan, coll, true);
runRewritesTest({t: -1}, null, null, backwardCollscan, coll, false);

// Indexed cases
runRewritesTest({t: 1}, {t: 1}, null, null, coll, true);
runRewritesTest({t: -1}, {t: -1}, {t: -1}, forwardIxscan, coll, true);
runRewritesTest({t: 1}, {t: 1}, {t: 1}, forwardIxscan, coll, true);
runRewritesTest({t: 1}, {t: -1}, {t: -1}, backwardIxscan, coll, false);
runRewritesTest({t: -1}, {t: 1}, {t: 1}, backwardIxscan, coll, false);
runRewritesTest({m: 1, t: -1}, {m: 1, t: -1}, {m: 1, t: -1}, forwardIxscan, metaColl, true);
runRewritesTest({m: -1, t: 1}, {m: -1, t: 1}, {m: -1, t: 1}, forwardIxscan, metaColl, true);
runRewritesTest({m: -1, t: -1}, {m: -1, t: -1}, {m: -1, t: -1}, forwardIxscan, metaColl, true);
runRewritesTest({m: 1, t: 1}, {m: 1, t: 1}, {m: 1, t: 1}, forwardIxscan, metaColl, true);

// Intermediary projects that don't modify sorted fields are allowed.
runRewritesTest(
    {m: 1, t: 1}, {m: 1, t: 1}, {m: 1, t: 1}, forwardIxscan, metaColl, true, [{$project: {a: 0}}]);
runRewritesTest({m: 1, t: 1}, {m: 1, t: 1}, {m: 1, t: 1}, forwardIxscan, metaColl, true, [
    {$project: {m: 1, t: 1}}
]);
runRewritesTest(
    {t: 1}, {t: 1}, {t: 1}, forwardIxscan, metaColl, true, [{$project: {m: 0, _id: 0}}]);
runRewritesTest(
    {'m.b': 1, t: 1}, {'m.b': 1, t: 1}, {'m.b': 1, t: 1}, forwardIxscan, metaCollSubFields, true, [
        {$project: {'m.a': 0}}
    ]);

// Test multiple meta fields
let metaIndexObj = Object.assign({}, ...subFields.map(field => ({[`m.${field}`]: 1})));
Object.assign(metaIndexObj, {t: 1});
runRewritesTest(metaIndexObj, metaIndexObj, metaIndexObj, forwardIxscan, metaCollSubFields, true);
runRewritesTest(metaIndexObj, metaIndexObj, metaIndexObj, forwardIxscan, metaCollSubFields, true, [
    {$project: {m: 1, t: 1}}
]);

// Check sort-limit optimization.
runRewritesTest({t: 1}, {t: 1}, {t: 1}, null, coll, true, [], [{$limit: 10}]);

// Check set window fields is optimized as well.
// Since {k: 1} cannot provide a bounded sort we know if there's a bounded sort it comes form
// setWindowFields.
runRewritesTest({k: 1}, {m: 1, t: 1}, {m: 1, t: 1}, null, metaColl, true, [], [
    {$setWindowFields: {partitionBy: "$m", sortBy: {t: 1}, output: {arr: {$max: "$t"}}}}
]);
// Test that when a collection scan is hinted, we rewrite to bounded sort even if the hint of
// the direction is opposite to the sort.
runRewritesTest({t: -1}, null, {$natural: 1}, backwardCollscan, coll, false, [], []);
runRewritesTest({t: 1}, null, {$natural: -1}, forwardCollscan, coll, true, [], []);
