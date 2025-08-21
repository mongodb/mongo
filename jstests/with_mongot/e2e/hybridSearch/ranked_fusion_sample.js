/**
 * Tests that the $sample stage works reasonably within $rankFusion.
 *
 * @tags: [featureFlagSearchHybridScoringFull, requires_fcv_82]
 */

import {assertErrorCode} from "jstests/aggregation/extras/utils.js";

const coll = db[jsTestName()];
coll.drop();
const bulk = coll.initializeUnorderedBulkOp();
const nDocs = 10000;
for (let i = 0; i < nDocs; i++) {
    bulk.insert({i, cowbell: true, "more?": i == 9999});
}
assert.commandWorked(bulk.execute());

// $sample by itself is not a ranked pipeline.
assertErrorCode(coll, [{$rankFusion: {input: {pipelines: {sampled: [{$sample: {size: nDocs}}]}}}}], 9191100);

// Make sure a $sample which is eligible to be optimized into a random cursor is also rejected.
assertErrorCode(coll, [{$rankFusion: {input: {pipelines: {sampled: [{$sample: {size: 4}}]}}}}], 9191100);

// Test that it is rejected in any position.
assertErrorCode(
    coll,
    [
        {
            $rankFusion: {
                input: {
                    pipelines: {
                        alphabetically_first: [{$match: {cowbell: true}}],
                        sampled: [{$sample: {size: 4}}],
                    },
                },
            },
        },
    ],
    9191100,
);

// Test that it fails if only the second pipeline is a valid scored selection pipeline.
assertErrorCode(
    coll,
    [
        {
            $rankFusion: {
                input: {
                    pipelines: {
                        alphabetically_first: [{$match: {"more?": true}}],
                        sampled: [{$sample: {size: 4}}, {$sort: {i: 1}}],
                    },
                },
            },
        },
    ],
    9191100,
);

// Test that it is allowed if there is an explicit $sort.
assert.commandWorked(
    db.runCommand({
        aggregate: jsTestName(),
        pipeline: [{$rankFusion: {input: {pipelines: {sampled: [{$sample: {size: 4}}, {$sort: {i: 1}}]}}}}],
        cursor: {},
    }),
);
