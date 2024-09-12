/**
 * Tests accumulators (e.g. $concatArrays) that are not supported in API Version 1.
 *
 * @tags: [featureFlagArrayAccumulators, uses_api_parameters, requires_fcv_81]
 */

import {APIVersionHelpers} from "jstests/libs/api_version_helpers.js";

const collName = "api_version_accumulators";
const viewName = collName + "_view";

const coll = db[collName];
coll.drop();
assert.commandWorked(coll.insert({a: [1]}));

const unstableAccumulatorPipelines = [
    [{$group: {_id: null, field: {$concatArrays: '$a'}}}],
    [{$setWindowFields: {output: {field: {$concatArrays: '$a'}}}}]
];

for (const pipeline of unstableAccumulatorPipelines) {
    // Assert error thrown when running a pipeline with accumulators not in API Version 1.
    APIVersionHelpers.assertAggregateFailsWithAPIStrict(
        pipeline, collName, ErrorCodes.APIStrictError);

    // Assert error thrown when creating a view on a pipeline with accumulators not in APIv1
    APIVersionHelpers.assertViewFailsWithAPIStrict(pipeline, viewName, collName);
}
