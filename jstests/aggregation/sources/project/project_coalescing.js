/**
 * Test that coalesced $projects return expected fields.
 *
 * @tags: []
 */
import {fc} from "jstests/third_party/fast_check/fc-3.1.0.js";

const coll = db.project_coalescing;
coll.drop();
// $project coalescing only operates on top-level fields.
assert.commandWorked(coll.insert({_id: 1, a: 1, b: 1, c: 1, d: 1}));

// Now we model what our pipelines will look like.
// 'arb' is short for arbitrary. This is a model of an arbitrary field.
const fieldArb = fc.constantFrom('_id', 'a', 'b', 'c', 'd');

// {$project: {_id: 0/1, a: 0/1, ...}}
const projectFieldsArb = fc.uniqueArray(fieldArb, {minLength: 1, maxLength: 5});
// To make a valid $project, we need a list of fields, a boolean for whether the _id field is
// included, and a boolean for whether the non-_id fields are included (if the projection inclusive)
const projectArb = fc.tuple(projectFieldsArb, fc.boolean(), fc.boolean())
                       .map(([fields, idIncluded, isInclusive]) => {
                           const projectList = {};
                           for (const field of fields) {
                               projectList[field] = field === '_id' ? idIncluded : isInclusive;
                           }
                           return {$project: projectList};
                       });

// A pipeline is [$project, $project, ...]
const pipelineModel = fc.array(projectArb, {minLength: 1, maxLength: 5});

// You can get some examples of what pipelineModel looks like using fc.sample
// jsTestLog(fc.sample(pipelineModel))

// We can provide a list of example cases for fast-check to test before generating random cases. In
// this case we provide an example that used to be a bug, from SERVER-91405. The bug repros with the
// pipeline [{$project: {a: 0}}, {$project: {a: 0}}]
const examples = [[[{$project: {a: 0}}, {$project: {a: 0}}]]];

// Get a full specification of what fields are included and excluded by this $project, since we know
// the schema. This is used to calculate the correct answer.
function getFullSpec(projSpec) {
    const projKeys = Object.keys(projSpec);
    projKeys.sort();
    // If only _id is specified, it indicates the full inclusive/exclusiveness.
    if (projKeys.length === 1 && projKeys[0] === '_id') {
        if (projSpec._id) {
            return {_id: 1, a: 0, b: 0, c: 0, d: 0};
        } else {
            return {_id: 0, a: 1, b: 1, c: 1, d: 1};
        }
    }

    // All non-id fields must have the same inclusivity/exclusivity to be a valid $project. _id will
    // always be at the front since we sorted the keys, so let's just take the last one.
    const nonIdField = projKeys[projKeys.length - 1];
    const isInclusive = projSpec[nonIdField];
    const fullSpec =
        isInclusive ? {_id: 1, a: 0, b: 0, c: 0, d: 0} : {_id: 1, a: 1, b: 1, c: 1, d: 1};
    for (const key of projKeys) {
        fullSpec[key] = projSpec[key];
    }
    return fullSpec;
}

fc.assert(fc.property(pipelineModel, (pipeline) => {
    // By default, all fields will be included. Keep track of what fields we should see at each
    // stage of the pipeline, and then we'll know what to expect in the result.
    const expectedFields = {_id: 1, a: 1, b: 1, c: 1, d: 1};
    for (const stage of pipeline) {
        const fullSpec = getFullSpec(stage['$project']);
        for (const key of Object.keys(fullSpec)) {
            expectedFields[key] &= fullSpec[key];
        }
    }

    const results = coll.aggregate(pipeline).toArray();
    assert.eq(results.length, 1);

    // Check the result against what we expect.
    const resultKeys = Object.keys(results[0]);
    for (const key of Object.keys(expectedFields)) {
        if (expectedFields[key]) {
            assert(resultKeys.includes(key));
        } else {
            assert(!resultKeys.includes(key));
        }
    }
}), {seed: 5, numRuns: 500, examples});
