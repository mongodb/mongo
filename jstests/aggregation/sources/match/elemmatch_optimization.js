/**
 * Tests for optimization of aggregation pipelines with $match with $elemMatch predicate.
 * The optimization can swap such $match stage with stages from which there is no dependency. As a
 * result multiple $match stages can be merged and the $elemMatch predicate can be used for tighter
 * index scan bounds. This optimization was enabled after SERVER-73914.
 *
 * @tags : [
 *  # Tests the explain output, so does not work when wrapped in a facet and during stepdowns.
 *  do_not_wrap_aggregations_in_facets,
 *  does_not_support_stepdowns,
 *  requires_pipeline_optimization,
 *  requires_fcv_70,
 * ]
 */

import {
    getPlanStages,
    getWinningPlanFromExplain,
} from "jstests/libs/analyze_plan.js";

const coll = db[jsTestName()];
coll.drop();

const size = 30;

const docFields = {
    a: '07',
    b: 1.5,
    arrayField: [{"label": "abc", "type": "X"}, {"label": "123", "type": "Y"}]
};
const docFields1 = {
    a: '07',
    b: 4.8,
    arrayField: [{"label": "xyz", "type": "X"}, {"label": "123", "type": "XY"}]
};
const docFields2 = {
    a: '08',
    arrayField: [{"label": "abc", "type": "X"}, {"label": "123", "type": "Y"}]
};

let bulk = coll.initializeUnorderedBulkOp();
for (let i = 0; i < size; i++) {
    bulk.insert({_id: i, ...docFields});
    bulk.insert({_id: i + size, ...docFields1});
    bulk.insert({_id: i + 2 * size, ...docFields2});
}
assert.commandWorked(bulk.execute());

assert.commandWorked(coll.createIndex({a: 1, "arrayField.label": 1}));

function assertIndexBounds(pipeline, expectedBounds) {
    const explain = coll.explain().aggregate(pipeline);
    const plan = getWinningPlanFromExplain(explain);
    const ixscans = getPlanStages(plan, 'IXSCAN');
    assert.eq(ixscans.length, 1, 'Plan is missing IXSCAN stage: ' + tojson(plan));

    assert.eq(
        ixscans[0].indexBounds,
        expectedBounds,
        `Expected bounds of ${tojson(expectedBounds)} but got ${tojson(ixscans[0].indexBounds)}`);
}

let pipeline = [
    {$match: {a: '07'}},
    {$sort: {a: 1}},
    {"$match": {"arrayField": {"$elemMatch": {"label": {"$eq": "abc"}}}}}
];
let expectedBounds = {"a": ["[\"07\", \"07\"]"], "arrayField.label": ["[\"abc\", \"abc\"]"]};
assertIndexBounds(pipeline, expectedBounds);

pipeline = [
    {$match: {a: '07'}},
    {$project: {a: 1, myArray: "$arrayField"}},
    {"$match": {"myArray": {"$elemMatch": {"label": {"$eq": "abc"}}}}}
];
assertIndexBounds(pipeline, expectedBounds);

// $elemMatch predicate provides bounds for the prefix of the index.
assert.commandWorked(coll.createIndex({"arrayField.type": 1, b: 1}));
pipeline = [
    {$match: {b: 4.8}},
    {$project: {a: 1, b: 1, myArray: "$arrayField"}},
    {"$match": {"myArray": {"$elemMatch": {"type": {"$eq": "X"}}}}}
];
expectedBounds = {
    "arrayField.type": ["[\"X\", \"X\"]"],
    "b": ["[4.8, 4.8]"]
};
assertIndexBounds(pipeline, expectedBounds);
