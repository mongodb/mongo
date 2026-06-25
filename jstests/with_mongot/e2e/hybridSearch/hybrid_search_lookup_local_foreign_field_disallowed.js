/**
 * Tests that $scoreFusion and $rankFusion inside a $lookup subpipeline are rejected when the
 * localField/foreignField join syntax is used.
 *
 * TODO SERVER-121094 Remove this test when the feature flag is removed, since
 * featureFlagExtensionsInsideHybridSearch enables this functionality.
 *
 * @tags: [
 *   featureFlagSearchHybridScoringFull,
 *   featureFlagExtensionsInsideHybridSearch_incompatible,
 *   requires_fcv_82,
 * ]
 */

import {describe, it} from "jstests/libs/mochalite.js";

const collName = jsTestName();
const coll = db.getCollection(collName);
assert.commandWorked(coll.insert({_id: 0, x: 1}));

function makeScoreFusionPipeline() {
    return [
        {
            $scoreFusion: {
                input: {
                    pipelines: {
                        a: [{$search: {text: {query: "foo", path: "x"}}}],
                    },
                    normalization: "sigmoid",
                },
                combination: {method: "avg"},
            },
        },
    ];
}

function makeRankFusionPipeline() {
    return [
        {
            $rankFusion: {
                input: {
                    pipelines: {
                        a: [{$search: {text: {query: "foo", path: "x"}}}],
                    },
                },
            },
        },
    ];
}

try {
    describe("$scoreFusion and $rankFusion in $lookup with localField/foreignField syntax are disallowed", function () {
        it("$scoreFusion: fails with 12982600 when $lookup uses localField/foreignField", function () {
            assert.commandFailedWithCode(
                db.runCommand({
                    aggregate: collName,
                    pipeline: [
                        {
                            $lookup: {
                                from: collName,
                                localField: "_id",
                                foreignField: "_id",
                                pipeline: makeScoreFusionPipeline(),
                                as: "results",
                            },
                        },
                    ],
                    cursor: {},
                }),
                12982600,
            );
        });

        it("$rankFusion: fails with 12982600 when $lookup uses localField/foreignField", function () {
            assert.commandFailedWithCode(
                db.runCommand({
                    aggregate: collName,
                    pipeline: [
                        {
                            $lookup: {
                                from: collName,
                                localField: "_id",
                                foreignField: "_id",
                                pipeline: makeRankFusionPipeline(),
                                as: "results",
                            },
                        },
                    ],
                    cursor: {},
                }),
                12982600,
            );
        });
    });
} finally {
    coll.drop();
}
