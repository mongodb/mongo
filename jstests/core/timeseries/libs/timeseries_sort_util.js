/**
 * Helpers for testing timeseries with sort.
 */

import {getExplainedPipelineFromAggregation} from "jstests/aggregation/extras/utils.js";
import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";
import {getAggPlanStages} from "jstests/libs/query/analyze_plan.js";

export const forwardCollscan = {
    stage: "COLLSCAN",
    direction: "forward"
};
export const backwardCollscan = {
    stage: "COLLSCAN",
    direction: "backward"
};
export const forwardIxscan = {
    stage: "IXSCAN",
    direction: "forward"
};
export const backwardIxscan = {
    stage: "IXSCAN",
    direction: "backward"
};

export const setupColl = (coll, collName, usesMeta, subFields = null) => {
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

const hasInternalBoundedSort = (pipeline) =>
    pipeline.some((stage) => stage.hasOwnProperty("$_internalBoundedSort"));

const findFirstMatch = (pipeline) => pipeline.find(stage => stage.hasOwnProperty("$match"));

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
export const runRewritesTest = (sortSpec,
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
    assert.docEq(ogResults, optResults);
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
            bucketSpanMatch, foundMatch, 'Expected an extra $match to check the bucket span');
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

export const runDoesntRewriteTest =
    (sortSpec, createIndex, hint, testColl, intermediaryStages = []) => {
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
