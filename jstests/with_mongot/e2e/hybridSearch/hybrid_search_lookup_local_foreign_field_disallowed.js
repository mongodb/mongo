/**
 * Tests that $scoreFusion and $rankFusion inside a $lookup subpipeline are rejected with error
 * 12982600 when the localField/foreignField join syntax is used and
 * featureFlagExtensionsInsideHybridSearch is disabled.
 *
 * TODO SERVER-121094 Remove this test when the feature flag is removed.
 *
 * @tags: [
 *   assumes_stable_shard_list,
 *   featureFlagSearchHybridScoringFull,
 *   requires_fcv_82,
 * ]
 */

import {
    getParameter,
    setParameterOnAllNonConfigNodes,
} from "jstests/noPassthrough/libs/server_parameter_helpers.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";

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

describe("$scoreFusion and $rankFusion in $lookup with localField/foreignField syntax are disallowed", function () {
    const kFlagName = "featureFlagExtensionsInsideHybridSearch";
    let prevFlagValue;

    before(function () {
        prevFlagValue = getParameter(db.getMongo(), kFlagName).value;
        setParameterOnAllNonConfigNodes(db.getMongo(), kFlagName, false);
    });

    after(function () {
        coll.drop();
        setParameterOnAllNonConfigNodes(db.getMongo(), kFlagName, prevFlagValue);
    });

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
