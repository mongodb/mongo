/**
 * Tests hybrid search syntax that isn't supported in API Version 1.
 *
 * @tags: [featureFlagSearchHybridScoringPrerequisites, uses_api_parameters, requires_fcv_81]
 */

import {APIVersionHelpers} from "jstests/libs/api_version_helpers.js";

const collName = "api_version_accumulators";
const viewName = collName + "_view";

const coll = db[collName];
coll.drop();
assert.commandWorked(coll.insert({a: [1]}));

const unstableHybridSearchPipelines = [
    [{$project: {output: {$sigmoid: 0}}}],
];

for (const pipeline of unstableHybridSearchPipelines) {
    // Assert error thrown when running a pipeline with syntax not in API Version 1.
    APIVersionHelpers.assertAggregateFailsWithAPIStrict(
        pipeline, collName, ErrorCodes.APIStrictError);

    // If we don't specify apiStrict then the pipeline succeeds.
    APIVersionHelpers.assertAggregateSucceedsAPIVersionWithoutAPIStrict(pipeline, collName);

    // Assert error thrown when creating a view on a pipeline with syntax not in APIv1.
    APIVersionHelpers.assertViewFailsWithAPIStrict(pipeline, viewName, collName);
}
