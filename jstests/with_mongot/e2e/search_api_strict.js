/**
 * Verifies that $search and $vectorSearch are not part of the Stable API. Runs in
 * both the legacy mongot_e2e_* suites and the with_mongot_extension_* suites, so it asserts the
 * same behavior regardless of whether each stage is implemented by the server or by the mongot
 * extension.
 */

import {describe, it} from "jstests/libs/mochalite.js";
import {APIVersionHelpers} from "jstests/libs/query_optimization/api_version_helpers.js";

const collName = jsTestName();
const kStages = [
    {$search: {text: {query: "hello", path: "body"}}},
    {$vectorSearch: {queryVector: [1.0, 2.0], path: "embedding", numCandidates: 10, limit: 5}},
];

describe("search stages with apiStrict", function () {
    for (const stage of kStages) {
        const stageName = Object.keys(stage)[0];

        it(`accepts ${stageName} in API Version 1 without apiStrict`, function () {
            APIVersionHelpers.assertAggregateSucceedsAPIVersionWithoutAPIStrict(
                db,
                [stage],
                collName,
            );
        });

        it(`rejects ${stageName} with apiStrict: true`, function () {
            APIVersionHelpers.assertAggregateFailsWithAPIStrict(
                db,
                [stage],
                collName,
                ErrorCodes.APIStrictError,
            );
        });

        it(`rejects ${stageName} with apiStrict: true in a subpipeline`, function () {
            APIVersionHelpers.assertAggregateFailsWithAPIStrict(
                db,
                [{$unionWith: {coll: collName, pipeline: [stage]}}],
                collName,
                ErrorCodes.APIStrictError,
            );
        });
    }
});
