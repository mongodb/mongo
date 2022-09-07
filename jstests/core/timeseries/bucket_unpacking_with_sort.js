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
 * ]
 */
(function() {
"use strict";

load("jstests/libs/fixture_helpers.js");             // For FixtureHelpers.
load("jstests/aggregation/extras/utils.js");         // For getExplainedPipelineFromAggregation.
load("jstests/core/timeseries/libs/timeseries.js");  // For TimeseriesTest
load("jstests/libs/analyze_plan.js");                // For getAggPlanStage

if (!TimeseriesTest.bucketUnpackWithSortEnabled(db.getMongo())) {
    jsTestLog("Skipping test because 'BucketUnpackWithSort' is disabled.");
    return;
}

const collName = "bucket_unpacking_with_sort";
const coll = db[collName];
const metaCollName = "bucket_unpacking_with_sort_with_meta";
const metaColl = db[metaCollName];
const metaCollSubFieldsName = "bucket_unpacking_with_sort_with_meta_sub";
const metaCollSubFields = db[metaCollSubFieldsName];
const geoCollName = 'bucket_unpacking_with_sort_geo';
const geoColl = db[geoCollName];
// Case-insensitive, and case-sensitive string collections.
const ciStringCollName = 'bucket_unpacking_with_sort_ci';
const ciStringColl = db[ciStringCollName];
const csStringCollName = 'bucket_unpacking_with_sort_cs';
const csStringColl = db[csStringCollName];
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
            metaVal = Object.assign({}, ...subFields.map(field => ({[field]: j})));
            metaVal.array = [1, 2];
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

    TimeseriesTest.ensureDataIsDistributedIfSharded(coll,
                                                    new Date((numberOfItemsPerBucket / 2) * 6000));
};

setupColl(coll, collName, false);
setupColl(metaColl, metaCollName, true);
setupColl(metaCollSubFields, metaCollSubFieldsName, true, subFields);
{
    geoColl.drop();
    // We'll only use the geo collection to test that the rewrite doesn't happen, so it doesn't
    // need to be big.
    assert.commandWorked(
        db.createCollection(geoCollName, {timeseries: {timeField: "t", metaField: "m"}}));
    // This polygon is big enough that a 2dsphere index on it is multikey.
    const area = {type: "Polygon", coordinates: [[[0, 0], [3, 6], [6, 1], [0, 0]]]};
    assert.commandWorked(geoColl.insert([
        // These two locations are far enough apart that a 2dsphere index on 'loc' is multikey.
        {t: ISODate('1970-01-01'), m: {area}, loc: [0, 0]},
        {t: ISODate('1970-01-01'), m: {area}, loc: [90, 0]},
    ]));
    assert.eq(db['system.buckets.' + geoCollName].count(), 1);
}
{
    // Create two collections, with the same data but different collation.

    const times = [
        ISODate('1970-01-01T00:00:00'),
        ISODate('1970-01-01T00:00:07'),
    ];
    let docs = [];
    for (const m of ['a', 'A', 'b', 'B'])
        for (const t of times)
            docs.push({t, m});

    csStringColl.drop();
    ciStringColl.drop();
    assert.commandWorked(db.createCollection(csStringCollName, {
        timeseries: {timeField: "t", metaField: "m"},
    }));
    assert.commandWorked(db.createCollection(ciStringCollName, {
        timeseries: {timeField: "t", metaField: "m"},
        collation: {locale: 'en_US', strength: 2},
    }));

    for (const coll of [csStringColl, ciStringColl]) {
        assert.commandWorked(coll.insert(docs));
        assert.eq(db['system.buckets.' + coll.getName()].count(), 4);
    }
}

const hasInternalBoundedSort = (pipeline) =>
    pipeline.some((stage) => stage.hasOwnProperty("$_internalBoundedSort"));

const findFirstMatch = (pipeline) => pipeline.find(stage => stage.hasOwnProperty("$match"));

const getWinningPlan = (explain) => {
    if (explain.hasOwnProperty("shards")) {
        for (const shardName in explain.shards) {
            return explain.shards[shardName].stages[0]["$cursor"].queryPlanner.winningPlan;
        }
    }
    return explain.stages[0]["$cursor"].queryPlanner.winningPlan;
};

const getAccessPathFromWinningPlan = (winningPlan) => {
    if (winningPlan.stage == "SHARDING_FILTER" || winningPlan.stage === "FETCH") {
        return getAccessPathFromWinningPlan(winningPlan.inputStage);
    } else if (winningPlan.stage === "COLLSCAN" || winningPlan.stage === "IXSCAN") {
        return winningPlan;
    }
};

const getAccessPath = (explain) => {
    return getAccessPathFromWinningPlan(getWinningPlan(explain));
};

const setup = (coll, createIndex = null) => {
    if (createIndex) {
        assert.commandWorked(coll.dropIndexes());
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
                         expectedAccessPath,
                         testColl,
                         precise,
                         intermediaryStages = [],
                         posteriorStages = []) => {
    jsTestLog(`runRewritesTest ${tojson({
        sortSpec,
        createIndex,
        hint,
        expectedAccessPath,
        testColl,
        precise,
        intermediaryStages,
        posteriorStages
    })}`);
    setup(testColl, createIndex);
    assert.neq(typeof precise, "object", `'precise' arg must be a boolean: ${tojson(precise)}`);

    const options = (hint) ? {hint: hint} : {};

    // Get results
    const optPipeline = [...intermediaryStages, {$sort: sortSpec}, ...posteriorStages];
    const optResults = testColl.aggregate(optPipeline, options).toArray();
    const optExplainFull = testColl.explain().aggregate(optPipeline, options);
    printjson({optExplainFull});

    const ogPipeline = [
        ...intermediaryStages,
        {$_internalInhibitOptimization: {}},
        {$sort: sortSpec},
        ...posteriorStages
    ];
    const ogResults = testColl.aggregate(ogPipeline, options).toArray();
    const ogExplainFull = testColl.explain().aggregate(ogPipeline, options);

    // Assert correct
    assert.docEq(optResults, ogResults);
    // Make sure we're not testing trivial / empty queries.
    assert.gt(ogResults.length, 0, 'Expected the queries in this test to have nonempty results');

    // Check contains stage
    const optExplain = getExplainedPipelineFromAggregation(
        db,
        testColl,
        optPipeline,
        {inhibitOptimization: false, hint: hint, postPlanningResults: true});
    assert(hasInternalBoundedSort(optExplain), optExplainFull);

    // Check doesn't contain stage
    const ogExplain = getExplainedPipelineFromAggregation(
        db,
        testColl,
        ogPipeline,
        {inhibitOptimization: false, hint: hint, postPlanningResults: true});
    assert(!hasInternalBoundedSort(ogExplain), ogExplainFull);

    // For some queries we expect to see an extra predicate, to defend against bucketMaxSpanSeconds
    // changing out from under us.
    const bucketSpanMatch = {
        $match: {
            $expr: {
                $lte: [
                    {$subtract: ["$control.max.t", "$control.min.t"]},
                    {$const: NumberLong(3600000)}
                ]
            },
        }
    };
    let foundMatch = findFirstMatch(optExplain);
    if (!precise) {
        assert.docEq(
            foundMatch, bucketSpanMatch, 'Expected an extra $match to check the bucket span');
    } else {
        // (We don't have a 'assert.notDocEq' helper, but docEq is 'eq' + 'sortDoc'.)
        assert.neq(sortDoc(foundMatch),
                   sortDoc(bucketSpanMatch),
                   'Did not expect an extra $match to check the bucket span');
    }

    if (expectedAccessPath) {
        const paths = getAggPlanStages(optExplainFull, expectedAccessPath.stage);
        for (const path of paths) {
            for (const key in expectedAccessPath) {
                assert.eq(path[key], expectedAccessPath[key]);
            }
        }
    }
};

const runDoesntRewriteTest = (sortSpec, createIndex, hint, testColl, intermediaryStages = []) => {
    jsTestLog(`runDoesntRewriteTest ${
        tojson({sortSpec, createIndex, hint, testColl, intermediaryStages})}`);
    setup(testColl, createIndex);

    const optPipeline = [
        ...intermediaryStages,
        {$sort: sortSpec},
    ];

    // Check doesn't contain stage
    const optExplain = getExplainedPipelineFromAggregation(
        db,
        testColl,
        optPipeline,
        {inhibitOptimization: false, hint: hint, postPlanningResults: true});
    const optExplainFull = testColl.explain().aggregate(optPipeline, {hint});
    const containsOptimization = hasInternalBoundedSort(optExplain);
    assert(!containsOptimization, optExplainFull);
};

const forwardCollscan = {
    stage: "COLLSCAN",
    direction: "forward"
};
const backwardCollscan = {
    stage: "COLLSCAN",
    direction: "backward"
};
// We drop all other indexes during runRewritesTest, so asserting that an IXSCAN is used is enough.
const forwardIxscan = {
    stage: "IXSCAN",
    direction: "forward"
};
const backwardIxscan = {
    stage: "IXSCAN",
    direction: "backward"
};

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

// Test point queries on metadata.

// Test point predicate on a single meta field.
for (const sort of [-1, +1]) {
    for (const m of [-1, +1]) {
        for (const t of [-1, +1]) {
            const index = {m, t};
            const expectedAccessPath = t === sort ? forwardIxscan : backwardIxscan;
            runRewritesTest({t: sort}, index, index, expectedAccessPath, metaColl, t === sort, [
                {$match: {m: 7}}
            ]);
            runRewritesTest({t: sort}, index, null, expectedAccessPath, metaColl, t === sort, [
                {$match: {m: 7}}
            ]);
        }
    }
}

// Test point predicate on multiple meta fields.
for (const sort of [-1, +1]) {
    for (const a of [-1, +1]) {
        for (const b of [-1, +1]) {
            for (const t of [-1, +1]) {
                for (const trailing of [{}, {x: 1, y: -1}]) {
                    const index = Object.merge({'m.a': a, 'm.b': b, t: t}, trailing);
                    const expectedAccessPath = t === sort ? forwardIxscan : backwardIxscan;
                    runRewritesTest({t: sort},
                                    index,
                                    index,
                                    expectedAccessPath,
                                    metaCollSubFields,
                                    t === sort,
                                    [{$match: {'m.a': 5, 'm.b': 5}}]);
                    runRewritesTest(
                        {t: sort}, index, null, expectedAccessPath, metaCollSubFields, t === sort, [
                            {$match: {'m.a': 5, 'm.b': 5}}
                        ]);
                }
            }
        }
    }
}

// Test mixed cases involving both a point predicate and compound sort.
// In all of these cases we have an index on {m.a, m.b, t}, and possibly some more trailing fields.
for (const ixA of [-1, +1]) {
    for (const ixB of [-1, +1]) {
        for (const ixT of [-1, +1]) {
            for (const ixTrailing of [{}, {x: 1, y: -1}]) {
                const ix = Object.merge({'m.a': ixA, 'm.b': ixB, t: ixT}, ixTrailing);

                // Test a point predicate on 'm.a' with a sort on {m.b, t}.
                // The point predicate lets us zoom in on a contiguous range of the index,
                // as if we were using an index on {constant, m.b, t}.
                for (const sortB of [-1, +1]) {
                    for (const sortT of [-1, +1]) {
                        const predicate = [{$match: {'m.a': 7}}];
                        const sort = {'m.b': sortB, t: sortT};

                        // 'sortB * sortT' is +1 if the sort has those fields in the same
                        // direction, -1 for opposite direction. 'b * t' says the same thing about
                        // the index key. The index and sort are compatible iff they agree on
                        // whether or not these two fields are in the same direction.
                        if (ixB * ixT === sortB * sortT) {
                            runRewritesTest(
                                sort, ix, ix, null, metaCollSubFields, ixT === sortT, predicate);
                            runRewritesTest(sort,
                                            ix,
                                            null,
                                            ixT === sortT ? forwardIxscan : backwardIxscan,
                                            metaCollSubFields,
                                            ixT === sortT,
                                            predicate);
                        } else {
                            runDoesntRewriteTest(sort, ix, ix, metaCollSubFields, predicate);
                        }
                    }
                }

                // Test a point predicate on 'm.b' with a sort on {m.a, t}.
                // This predicate does not select a contiguous range of the index, but it does
                // limit the scan to index entries that look like {m.a, constant, t}, which can
                // satisfy a sort on {m.a, t}.
                for (const sortA of [-1, +1]) {
                    for (const sortT of [-1, +1]) {
                        const sort = {'m.a': sortA, t: sortT};

                        // However, when there is no point predicate on 'm.a', the planner gives us
                        // a full index scan with no bounds on 'm.b'. Since our implementation
                        // looks at index bounds to decide whether to rewrite, we don't get the
                        // optimization in this case.
                        {
                            const predicate = [{$match: {'m.b': 7}}];
                            runDoesntRewriteTest(sort, ix, ix, metaCollSubFields, predicate);
                        }

                        // We do get the optimization if we add any range predicate to 'm.a',
                        // because that makes the planner generate index bounds: a range on 'm.a'
                        // and a single point on 'm.b'.
                        //
                        // As usual the index and sort must agree on whether m.a, t are
                        // in the same direction.
                        const predicate = [{$match: {'m.a': {$gte: -999, $lte: 999}, 'm.b': 7}}];
                        if (ixA * ixT === sortA * sortT) {
                            runRewritesTest(
                                sort, ix, ix, null, metaCollSubFields, ixT === sortT, predicate);
                            runRewritesTest(sort,
                                            ix,
                                            null,
                                            ixT === sortT ? forwardIxscan : backwardIxscan,
                                            metaCollSubFields,
                                            ixT === sortT,
                                            predicate);
                        } else {
                            runDoesntRewriteTest(sort, ix, ix, metaCollSubFields, predicate);
                        }
                    }
                }
            }
        }
    }
}

// Test some negative cases:
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

// Geo indexes are typically multikey, which prevents us from doing the rewrite.
{
    const indexes = [
        {t: 1, 'm.area': '2dsphere'},
        {'m.a': 1, t: 1, 'm.area': '2dsphere'},
        {t: 1, loc: '2dsphere'},
        {'m.a': 1, t: 1, loc: '2dsphere'},
    ];
    for (const ix of indexes)
        runDoesntRewriteTest({t: 1}, ix, ix, geoColl, [{$match: {'m.a': 7}}]);
}

// String collation affects whether an equality query is really a point query.
{
    // When the collation of the query matches the index, an equality predicate in the query
    // becomes a 1-point interval in the index bounds.
    runRewritesTest({t: 1}, {m: 1, t: 1}, {m: 1, t: 1}, forwardIxscan, csStringColl, true, [
        {$match: {m: 'a'}}
    ]);
    runRewritesTest({t: 1}, {m: 1, t: 1}, {m: 1, t: 1}, forwardIxscan, ciStringColl, true, [
        {$match: {m: 'a'}}
    ]);
    // When the collation doesn't match, then the equality predicate is not a 1-point interval
    // in the index.
    csStringColl.dropIndexes();
    ciStringColl.dropIndexes();
    assert.commandWorked(csStringColl.createIndex({m: 1, t: 1}, {
        collation: {locale: 'en_US', strength: 2},
    }));
    assert.commandWorked(ciStringColl.createIndex({m: 1, t: 1}, {
        collation: {locale: 'simple'},
    }));
    runDoesntRewriteTest({t: 1}, null, {m: 1, t: 1}, csStringColl, [{$match: {m: 'a'}}]);
    runDoesntRewriteTest({t: 1}, null, {m: 1, t: 1}, ciStringColl, [{$match: {m: 'a'}}]);
}
})();
