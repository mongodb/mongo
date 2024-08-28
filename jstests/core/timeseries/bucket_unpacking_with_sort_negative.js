/**
 * Test that the bucket unpacking with sorting rewrite is performed and doesn't cause incorrect
 * results to be created. This test is focused on negative scenarios when the rewrite doesn't occur.
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
    backwardIxscan,
    runDoesntRewriteTest,
    runRewritesTest,
    setupColl
} from "jstests/core/timeseries/libs/timeseries_sort_util.js";

const collName = "bucket_unpacking_with_sort_negative";
const coll = db[collName];
const metaCollName = "bucket_unpacking_with_sort_with_meta_negative";
const metaColl = db[metaCollName];
const metaCollSubFieldsName = "bucket_unpacking_with_sort_with_meta_sub_negative";
const metaCollSubFields = db[metaCollSubFieldsName];
const subFields = ["a", "b"];

setupColl(coll, collName, false);
setupColl(metaColl, metaCollName, true);
setupColl(metaCollSubFields, metaCollSubFieldsName, true, subFields);

// Negative tests and backwards cases
for (let m = -1; m < 2; m++) {
    for (let t = -1; t < 2; t++) {
        for (let k = -1; k < 2; k++) {
            printjson({"currently running": "the following configuration...", m: m, t: t, k: k});
            let sort = null;
            let createIndex = null;
            let hint = null;
            let usesMeta = null;
            if (k != 0) {
                // This is the case where we add an intermediary incompatible field.
                if (m == 0) {
                    if (t == 0) {
                        sort = {k: k};
                    } else {
                        sort = {k: k, t: t};
                    }
                } else {
                    if (t == 0) {
                        sort = {m: m, k: k};
                    } else {
                        sort = {m: m, k: k, t: t};
                    }
                }
                hint = sort;
                createIndex = sort;
                usesMeta = m != 0;
                runDoesntRewriteTest(sort, createIndex, hint, usesMeta ? metaColl : coll);
            } else {
                // This is the case where we do not add an intermediary incompatible field.
                // Instead we enumerate the ways that the index and sort could disagree.

                // For the meta case, negate the time order.
                // For the non-meta case, use a collscan with a negated order.
                if (m == 0) {
                    // Do not execute a test run.
                } else {
                    if (t == 0) {
                        // Do not execute a test run.
                    } else {
                        usesMeta = true;
                        sort = {m: m, t: t};
                        hint = {m: m, t: -t};
                        createIndex = hint;
                    }
                }

                if (sort) {
                    runDoesntRewriteTest(sort, createIndex, hint, usesMeta ? metaColl : coll);
                }

                sort = null;
                hint = null;
                createIndex = null;
                usesMeta = false;
                // For the meta case, negate the meta order.
                // For the non-meta case, use an index instead of a collscan.
                if (m == 0) {
                    // Do not execute a test run.
                } else {
                    if (t == 0) {
                        // Do not execute a test run.
                    } else {
                        usesMeta = true;
                        sort = {m: m, t: t};
                        hint = {m: -m, t: t};
                        createIndex = hint;
                    }
                }

                if (sort) {
                    runDoesntRewriteTest(sort, createIndex, hint, usesMeta ? metaColl : coll);
                }

                sort = null;
                hint = null;
                createIndex = null;
                usesMeta = false;
                // For the meta case, negate both meta and time.
                if (m == 0) {
                    if (t == 0) {
                        // Do not execute a test run.
                    } else {
                        // Do not execute -- we've exhausted relevant cases.
                    }
                } else {
                    if (t == 0) {
                        // Do not execute a test run.
                    } else {
                        usesMeta = true;
                        sort = {m: m, t: t};
                        hint = {m: -m, t: -t};
                        createIndex = hint;
                    }
                }

                if (sort)
                    runRewritesTest(
                        sort, createIndex, hint, backwardIxscan, usesMeta ? metaColl : coll);
            }
        }
    }
}

// Test that non-time, non-meta fields are not optimized.
runDoesntRewriteTest({foo: 1}, {foo: 1}, {foo: 1}, coll);
// Test that a meta-only sort does not use $_internalBoundedSort.
// (It doesn't need to: we can push down the entire sort.)
runDoesntRewriteTest({m: 1}, {m: 1}, {m: 1}, metaColl);

// Test mismatched meta paths don't produce the optimization.
runDoesntRewriteTest({m: 1, t: 1}, {"m.a": 1, t: 1}, {"m.a": 1, t: 1}, metaCollSubFields);
runDoesntRewriteTest(
    {"m.b": 1, t: 1}, {"m.a": 1, "m.b": 1, t: 1}, {"m.a": 1, "m.b": 1, t: 1}, metaCollSubFields);
runDoesntRewriteTest({"m.a": 1, t: 1}, {m: 1, t: 1}, {m: 1, t: 1}, metaCollSubFields);
runDoesntRewriteTest({"m.a": 1, "m.b": 1, t: 1}, {m: 1, t: 1}, {m: 1, t: 1}, metaCollSubFields);
// Test matched meta-subpaths with mismatched directions don't produce the optimization.
runDoesntRewriteTest({"m.a": 1, t: -1}, {"m.a": 1, t: 1}, {"m.a": 1, t: 1}, metaCollSubFields);

// Test intermediary projections that exclude the sorted fields don't produce the optimizaiton.
runDoesntRewriteTest({t: 1}, null, {$natural: 1}, metaColl, [{$project: {t: 0}}]);
runDoesntRewriteTest({t: 1}, null, {$natural: 1}, metaColl, [{$unset: 't'}]);
runDoesntRewriteTest({t: 1}, null, {$natural: 1}, metaColl, [{$set: {t: {$const: 5}}}]);
runDoesntRewriteTest({t: 1}, null, {$natural: 1}, metaColl, [{$set: {t: "$m.junk"}}]);

runDoesntRewriteTest({m: 1, t: 1}, {m: 1, t: 1}, {m: 1, t: 1}, metaColl, [{$project: {m: 0}}]);
runDoesntRewriteTest({m: 1, t: 1}, {m: 1, t: 1}, {m: 1, t: 1}, metaColl, [{$unset: 'm'}]);
runDoesntRewriteTest(
    {m: 1, t: 1}, {m: 1, t: 1}, {m: 1, t: 1}, metaColl, [{$set: {m: {$const: 5}}}]);
runDoesntRewriteTest({m: 1, t: 1}, {m: 1, t: 1}, {m: 1, t: 1}, metaColl, [{$set: {m: "$m.junk"}}]);

runDoesntRewriteTest({m: 1, t: 1}, {m: 1, t: 1}, {m: 1, t: 1}, metaColl, [{$project: {t: 0}}]);

runDoesntRewriteTest({m: 1, t: 1}, {m: 1, t: 1}, {m: 1, t: 1}, metaColl, [{$project: {a: 1}}]);

runDoesntRewriteTest({m: 1, t: 1}, {m: 1, t: 1}, {m: 1, t: 1}, metaColl, [{$project: {'m.a': 0}}]);

runDoesntRewriteTest({'m.a': 1, t: 1}, {'m.a': 1, t: 1}, {'m.a': 1, t: 1}, metaCollSubFields, [
    {$project: {'m': 0}}
]);

// The predicate must be an equality.
runDoesntRewriteTest(
    {t: 1}, {m: 1, t: 1}, {m: 1, t: 1}, metaColl, [{$match: {m: {$gte: 5, $lte: 6}}}]);
runDoesntRewriteTest(
    {t: 1}, {m: 1, t: 1}, {m: 1, t: 1}, metaColl, [{$match: {m: {$in: [4, 5, 6]}}}]);
// The index must not be multikey.
runDoesntRewriteTest({t: 1}, {'m.array': 1, t: 1}, {'m.array': 1, t: 1}, metaCollSubFields, [
    {$match: {'m.array': 123}}
]);
// Even if the multikey component is a trailing field, for simplicity we are not handling it.
runDoesntRewriteTest({t: 1},
                     {'m.a': 1, t: 1, 'm.array': 1},
                     {'m.a': 1, t: 1, 'm.array': 1},
                     metaCollSubFields,
                     [{$match: {'m.a': 7}}]);

// Test that a pipeline with the renamed time field by $addFields or $project will not be rewritten.
// In this case, the new 't' fields hide the timeField 't' and so the $sort that follows the
// $addFields or $project will not sort data by 't' and can't be rewritten into a bounded sort.
runDoesntRewriteTest({t: 1}, null, {}, coll, [{$addFields: {t: "$a"}}]);
runDoesntRewriteTest({t: 1}, null, {}, coll, [{$project: {t: "$a"}}]);
