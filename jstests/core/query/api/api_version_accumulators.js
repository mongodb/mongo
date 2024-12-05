/**
 * Tests accumulators (e.g. $concatArrays and $setUnion) that are not supported in API Version 1.
 *
 * @tags: [uses_api_parameters, requires_fcv_81]
 */

import {APIVersionHelpers} from "jstests/libs/api_version_helpers.js";

const collName = "api_version_accumulators";
const viewName = collName + "_view";

const coll = db[collName];
coll.drop();
assert.commandWorked(coll.insert({_id: 1, a: [1]}));

const stableAccumulatorPipelines = [
    [{$group: {_id: null, field: {$concatArrays: '$a'}}}],
    [{$setWindowFields: {output: {field: {$concatArrays: '$a'}}}}],
    [{$bucket: {groupBy: "$_id", boundaries: [0, 5], output: {nums: {$concatArrays: "$a"}}}}],
    [{$bucketAuto: {groupBy: "$_id", buckets: 2, output: {nums: {$concatArrays: "$a"}}}}],
    [{$group: {_id: null, field: {$setUnion: '$a'}}}],
    [{$setWindowFields: {output: {field: {$setUnion: '$a'}}}}],
    [{$bucket: {groupBy: "$_id", boundaries: [0, 5], output: {nums: {$setUnion: "$a"}}}}],
    [{$bucketAuto: {groupBy: "$_id", buckets: 2, output: {nums: {$setUnion: "$a"}}}}],
];

for (const pipeline of stableAccumulatorPipelines) {
    // Assert success when running a pipeline with accumulators added to version 1.
    APIVersionHelpers.assertAggregateSucceedsWithAPIStrict(pipeline, collName);

    // Assert that if we don't specify apiStrict then the pipeline still succeeds.
    APIVersionHelpers.assertAggregateSucceedsAPIVersionWithoutAPIStrict(pipeline, collName);

    // Assert success when creating a view on a pipeline with accumulators in APIv1
    APIVersionHelpers.assertViewSucceedsWithAPIStrict(pipeline, viewName, collName);
}
