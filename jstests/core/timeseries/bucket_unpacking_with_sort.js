/**
 * Test that the bucket unpacking with sorting rewrite is performed and doesn't cause incorrect
 * results to be created.
 *
 * @tags: [
 *     requires_fcv_61,
 *     # We need a timeseries collection.
 *     assumes_no_implicit_collection_creation_after_drop,
 *     # Bounded sorter is currently broken w/ sharding.
 *     assumes_unsharded_collection,
 *     # Cannot insert into a time-series collection in a multi-document transaction.
 *     does_not_support_transactions,
 *     # Refusing to run a test that issues an aggregation command with explain because it may
 *     # return incomplete results if interrupted by a stepdown.
 *     does_not_support_stepdowns,
 *     # This complicates aggregation extraction.
 *     do_not_wrap_aggregations_in_facets,
 *     # We need a timeseries collection.
 *     requires_timeseries,
 *     # Explain of a resolved view must be executed by mongos.
 *     directly_against_shardsvrs_incompatible,
 * ]
 */
(function() {
"use strict";

const featureEnabled =
    assert.commandWorked(db.adminCommand({getParameter: 1, featureFlagBucketUnpackWithSort: 1}))
        .featureFlagBucketUnpackWithSort.value;
if (!featureEnabled) {
    jsTestLog("Skipping test because the BUS feature flag is disabled");
    return;
}

load("jstests/libs/fixture_helpers.js");      // For FixtureHelpers.
load("jstests/aggregation/extras/utils.js");  // For getExplainedPipelineFromAggregation.
const collName = "bucket_unpacking_with_sort";
const coll = db[collName];
const metaCollName = "bucket_unpacking_with_sort_with_meta";
const metaColl = db[metaCollName];
const metaCollSubFieldsName = "bucket_unpacking_with_sort_with_meta_sub";
const metaCollSubFields = db[metaCollSubFieldsName];
const subFields = ["a", "b"];

const setupColl = (coll, collName, usesMeta, subFields = null) => {
    coll.drop();
    if (usesMeta) {
        db.createCollection(collName, {timeseries: {timeField: "t", metaField: "m"}});
    } else {
        db.createCollection(collName, {timeseries: {timeField: "t"}});
    }
    // Create a few buckets.
    let meta = usesMeta ? 10 : 1;
    let docs = [];
    let numberOfItemsPerBucket =
        db.adminCommand({getParameter: 1, timeseriesBucketMaxCount: 1}).timeseriesBucketMaxCount;
    assert(Number.isInteger(numberOfItemsPerBucket));
    for (let j = 0; j < meta; ++j) {
        let metaVal = j;
        if (subFields) {
            metaVal = subFields.reduce((memo, field) => {
                return Object.assign(Object.assign({}, memo), {[field]: j});
            }, {});
        }
        for (let i = 0; i < numberOfItemsPerBucket; ++i) {
            docs.push({m: metaVal, t: new Date(i * 6000)});
        }
        // Because the max bucket span is 3600 * 1000 milliseconds, we know that the above will have
        // generated two buckets. Since we are now going back in time to before the minimum
        // timestamp of the second bucket, we'll open a third and fourth bucket below. Crucially
        // will overlap with the first two buckets.
        for (let i = 0; i < numberOfItemsPerBucket; ++i) {
            docs.push({m: metaVal, t: new Date(i * 6000 + 3000)});
        }
    }
    assert.commandWorked(coll.insert(docs));
};

setupColl(coll, collName, false);
setupColl(metaColl, metaCollName, true);
setupColl(metaCollSubFields, metaCollSubFieldsName, true, subFields);

// For use in reductions.
// Takes a memo (accumulator value) and a stage and returns if the stage was an internal bounded
// sort or the memo was true.
const stageIsInternalBoundedSort = (stage) => {
    return stage.hasOwnProperty("$_internalBoundedSort");
};

const hasInternalBoundedSort = (pipeline) => pipeline.some(stageIsInternalBoundedSort);

const getIfMatch = (memo, stage) => {
    if (memo)
        return memo;
    else if (stage.hasOwnProperty("$match"))
        return stage;
    else
        return null;
};

const findFirstMatch = (pipeline) => pipeline.reduce(getIfMatch, null);

const setup = (coll, createIndex = null) => {
    // Create index.
    if (createIndex) {
        assert.commandWorked(coll.createIndex(createIndex));
    }
};

/**
 * This test creates a time-series collection, inserts data (with multiple overlapping buckets), and
 * tests that the BUS optimization is performed and produces correct results.
 *
 * `sortSpec` is the sort that we wish to have optimized.
 * `createIndex` is the index that we need to create in order to perform the optimization.
 * It defaults to null which signals that we don't create an index.
 * `hint` is the hint that we specify in order to produce the optimization.
 * traversing a min (resp. max) field index on a descending (resp. ascending) sort.
 * `testColl` is the collection to use.
 */
const runRewritesTest = (sortSpec,
                         createIndex,
                         hint,
                         testColl,
                         precise,
                         intermediaryStages = [],
                         posteriorStages = []) => {
    setup(testColl, createIndex);

    const options = (hint) ? {hint: hint} : {};

    const match = {$match: {t: {$lt: new Date("2022-01-01")}}};
    // Get results
    const optPipeline = [match, ...intermediaryStages, {$sort: sortSpec}, ...posteriorStages];
    const optResults = testColl.aggregate(optPipeline, options).toArray();
    const optExplainFull = testColl.explain().aggregate(optPipeline, options);

    const ogPipeline = [
        match,
        ...intermediaryStages,
        {$_internalInhibitOptimization: {}},
        {$sort: sortSpec},
        ...posteriorStages
    ];
    const ogResults = testColl.aggregate(ogPipeline, options).toArray();
    const ogExplainFull = testColl.explain().aggregate(ogPipeline, options);

    // Assert correct
    assert.docEq(optResults, ogResults, {optResults: optResults, ogResults: ogResults});

    // Check contains stage
    const optExplain = getExplainedPipelineFromAggregation(
        db, testColl, optPipeline, {inhibitOptimization: false, hint: hint});
    assert(hasInternalBoundedSort(optExplain), optExplainFull);

    // Check doesn't contain stage
    const ogExplain = getExplainedPipelineFromAggregation(
        db, testColl, ogPipeline, {inhibitOptimization: false, hint: hint});
    assert(!hasInternalBoundedSort(ogExplain), ogExplainFull);

    let foundMatch = findFirstMatch(optExplain);
    if (!precise) {
        assert.docEq(foundMatch, {
            $match: {
                $expr:
                    {$lte: [{$subtract: ["$control.max.t", "$control.min.t"]}, {$const: 3600000}]}
            }
        });
    } else {
        assert.docEq(foundMatch, match);
    }
};

const runDoesntRewriteTest = (sortSpec, createIndex, hint, testColl, intermediaryStages = []) => {
    setup(testColl, createIndex);

    const optPipeline = [
        {$match: {t: {$lt: new Date("2022-01-01")}}},
        ...intermediaryStages,
        {$sort: sortSpec},
    ];

    // Check doesn't contain stage
    const optExplain = getExplainedPipelineFromAggregation(
        db, testColl, optPipeline, {inhibitOptimization: false, hint: hint});
    const optExplainFull = testColl.explain().aggregate(optPipeline, {hint});
    const containsOptimization = optExplain.reduce(stageIsInternalBoundedSort, false);
    assert(!containsOptimization, optExplainFull);
};

// Collscan cases
runRewritesTest({t: 1}, null, null, coll, true);
runRewritesTest({t: -1}, null, {$natural: -1}, coll, false);

// Indexed cases
runRewritesTest({t: 1}, {t: 1}, {t: 1}, coll, true);
runRewritesTest({t: -1}, {t: -1}, {t: -1}, coll, true);
runRewritesTest({t: 1}, {t: 1}, {t: 1}, coll, true);
runRewritesTest({m: 1, t: -1}, {m: 1, t: -1}, {m: 1, t: -1}, metaColl, true);
runRewritesTest({m: -1, t: 1}, {m: -1, t: 1}, {m: -1, t: 1}, metaColl, true);
runRewritesTest({m: -1, t: -1}, {m: -1, t: -1}, {m: -1, t: -1}, metaColl, true);
runRewritesTest({m: 1, t: 1}, {m: 1, t: 1}, {m: 1, t: 1}, metaColl, true);

// Intermediary projects that don't modify sorted fields are allowed.
runRewritesTest({m: 1, t: 1}, {m: 1, t: 1}, {m: 1, t: 1}, metaColl, true, [{$project: {a: 0}}]);
runRewritesTest(
    {m: 1, t: 1}, {m: 1, t: 1}, {m: 1, t: 1}, metaColl, true, [{$project: {m: 1, t: 1}}]);

// Test multiple meta fields
let metaIndexObj = subFields.reduce((memo, field) => {
    return Object.assign(Object.assign({}, memo), {[`m.${field}`]: 1});
}, {});
Object.assign(metaIndexObj, {t: 1});
runRewritesTest(metaIndexObj, metaIndexObj, metaIndexObj, metaCollSubFields, true);
runRewritesTest(
    metaIndexObj, metaIndexObj, metaIndexObj, metaCollSubFields, true, [{$project: {m: 1, t: 1}}]);

// Check sort-limit optimization.
runRewritesTest({t: 1}, {t: 1}, {t: 1}, coll, true, [], [{$limit: 10}]);

// Check set window fields is optimized as well.
// Since {k: 1} cannot provide a bounded sort we know if there's a bounded sort it comes form
// setWindowFields.
runRewritesTest({k: 1}, {m: 1, t: 1}, {m: 1, t: 1}, metaColl, true, [], [
    {$setWindowFields: {partitionBy: "$m", sortBy: {t: 1}, output: {arr: {$max: "$t"}}}}
]);

// Negative tests
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
                    if (t == 0) {
                        // Do not execute a test run.
                    } else {
                        sort = {t: t};
                        hint = {$natural: -t};
                    }
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

                if (sort)
                    runDoesntRewriteTest(sort, createIndex, hint, usesMeta ? metaColl : coll);

                sort = null;
                hint = null;
                createIndex = null;
                usesMeta = false;
                // For the meta case, negate the meta order.
                // For the non-meta case, use an index instead of a collscan.
                if (m == 0) {
                    if (t == 0) {
                        // Do not execute a test run.
                    } else {
                        sort = {t: t};
                        createIndex = {t: -t};
                        hint = createIndex;
                    }
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

                if (sort)
                    runDoesntRewriteTest(sort, createIndex, hint, usesMeta ? metaColl : coll);

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
                    runDoesntRewriteTest(sort, createIndex, hint, usesMeta ? metaColl : coll);
            }
        }
    }
}

// Test mismatched meta paths don't produce the optimization.
runDoesntRewriteTest({m: 1, t: 1}, {"m.a": 1, t: 1}, {"m.a": 1, t: 1}, metaCollSubFields);
runDoesntRewriteTest(
    {"m.b": 1, t: 1}, {"m.a": 1, "m.b": 1, t: 1}, {"m.a": 1, "m.b": 1, t: 1}, metaCollSubFields);
runDoesntRewriteTest({"m.a": 1, t: 1}, {m: 1, t: 1}, {m: 1, t: 1}, metaCollSubFields);
runDoesntRewriteTest({"m.a": 1, "m.b": 1, t: 1}, {m: 1, t: 1}, {m: 1, t: 1}, metaCollSubFields);
// Test matched meta-subpaths with mismatched directions don't produce the optimization.
runDoesntRewriteTest({"m.a": 1, t: -1}, {"m.a": 1, t: 1}, {"m.a": 1, t: 1}, metaCollSubFields);
// Test intermediary projections that exclude the sorted fields don't produce the optimizaiton.
runDoesntRewriteTest({m: 1, t: 1}, {m: 1, t: 1}, {m: 1, t: 1}, metaColl, [{$project: {m: 0}}]);
runDoesntRewriteTest({m: 1, t: 1}, {m: 1, t: 1}, {m: 1, t: 1}, metaColl, [{$project: {t: 0}}]);
runDoesntRewriteTest({m: 1, t: 1}, {m: 1, t: 1}, {m: 1, t: 1}, metaColl, [{$project: {a: 1}}]);
runDoesntRewriteTest({m: 1, t: 1}, {m: 1, t: 1}, {m: 1, t: 1}, metaColl, [{$project: {'m.a': 0}}]);
runDoesntRewriteTest({'m.a': 1, t: 1}, {'m.a': 1, t: 1}, {'m.a': 1, t: 1}, metaCollSubFields, [
    {$project: {'m': 0}}
]);
})();
