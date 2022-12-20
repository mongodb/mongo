/**
 * Tests that the unpacking stage has correct unpacking behaviour when $match is pushed into it.
 *
 * @tags: [
 *   requires_timeseries,
 *   requires_fcv_62,
 *   does_not_support_stepdowns,
 *   directly_against_shardsvrs_incompatible,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/analyze_plan.js");  // For getAggPlanStages

const coll = db.timeseries_match_pushdown_with_project;
coll.drop();

const timeField = 'time';
const metaField = 'meta';
assert.commandWorked(db.createCollection(coll.getName(), {timeseries: {timeField, metaField}}));

const aTime = ISODate('2022-01-01T00:00:00');
assert.commandWorked(coll.insert([
    {[timeField]: aTime, a: 1, b: 1, _id: 1},
    {[timeField]: aTime, a: 2, b: 2, _id: 2},
    {[timeField]: aTime, a: 3, b: 3, _id: 3},
    {[timeField]: aTime, a: 4, b: 4, _id: 4},
    {[timeField]: aTime, a: 5, b: 5, _id: 5},
    {[timeField]: aTime, a: 6, b: 6, _id: 6},
    {[timeField]: aTime, a: 7, b: 7, _id: 7},
    {[timeField]: aTime, a: 8, b: 8, _id: 8},
    {[timeField]: aTime, a: 9, b: 9, _id: 9},
]));

/**
 * Runs a 'pipeline', asserts the bucket unpacking 'behaviour' (either include or exclude) is
 * expected.
 */
const runTest = function({pipeline, behaviour, expectedDocs}) {
    const explain = assert.commandWorked(coll.explain().aggregate(pipeline));
    const unpackStages = getAggPlanStages(explain, '$_internalUnpackBucket');
    assert.eq(1,
              unpackStages.length,
              "Should only have a single $_internalUnpackBucket stage: " + tojson(explain));
    const unpackStage = unpackStages[0].$_internalUnpackBucket;
    if (behaviour.include) {
        assert(unpackStage.include,
               "Unpacking stage must have 'include' behaviour: " + tojson(explain));
        assert.sameMembers(behaviour.include, unpackStage.include);
    }
    if (behaviour.exclude) {
        assert(unpackStage.exclude,
               "Unpacking stage must have 'exclude' behaviour: " + tojson(explain));
        assert.sameMembers(behaviour.exclude, unpackStage.exclude);
    }

    const docs = coll.aggregate([...pipeline, {$sort: {a: 1, b: 1, _id: 1}}]).toArray();
    assert.eq(docs.length, expectedDocs.length, "Incorrect docs: " + tojson(docs));
    docs.forEach((doc, i) => {
        assert.docEq(expectedDocs[i], doc, "Incorrect docs: " + tojson(docs));
    });
};

runTest({
    pipeline: [{$match: {a: {$gt: 5}}}, {$project: {b: 1}}],
    behaviour: {include: ['_id', 'a', 'b']},
    expectedDocs: [
        {b: 6, _id: 6},
        {b: 7, _id: 7},
        {b: 8, _id: 8},
        {b: 9, _id: 9},
    ],
});

runTest({
    pipeline: [{$match: {a: {$gt: 5}}}, {$project: {_id: 0, b: 1}}],
    behaviour: {include: ['a', 'b']},
    expectedDocs: [
        {b: 6},
        {b: 7},
        {b: 8},
        {b: 9},
    ],
});

runTest({
    pipeline: [{$match: {a: {$gt: 5}}}, {$project: {a: 1}}],
    behaviour: {include: ['_id', 'a']},
    expectedDocs: [
        {a: 6, _id: 6},
        {a: 7, _id: 7},
        {a: 8, _id: 8},
        {a: 9, _id: 9},
    ],
});

runTest({
    pipeline: [{$match: {a: {$gt: 5}}}, {$project: {_id: 0, a: 1}}],
    behaviour: {include: ['a']},
    expectedDocs: [
        {a: 6},
        {a: 7},
        {a: 8},
        {a: 9},
    ],
});

runTest({
    pipeline: [{$match: {a: {$gt: 5}}}, {$project: {a: 0}}],
    behaviour: {exclude: []},
    expectedDocs: [
        {[timeField]: aTime, b: 6, _id: 6},
        {[timeField]: aTime, b: 7, _id: 7},
        {[timeField]: aTime, b: 8, _id: 8},
        {[timeField]: aTime, b: 9, _id: 9},
    ],
});

runTest({
    pipeline: [{$match: {a: {$gt: 5}}}, {$project: {b: 0}}],
    behaviour: {exclude: []},
    expectedDocs: [
        {[timeField]: aTime, a: 6, _id: 6},
        {[timeField]: aTime, a: 7, _id: 7},
        {[timeField]: aTime, a: 8, _id: 8},
        {[timeField]: aTime, a: 9, _id: 9},
    ],
});
})();
