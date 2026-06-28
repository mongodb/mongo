/**
 * Tests non-mongot extension stages inside $rankFusion / $scoreFusion input pipelines when
 * featureFlagExtensionsInsideHybridSearch is enabled. Input pipelines may only contain selection
 * stages, and an extension is judged across every stage of its expansion.
 *
 * @tags: [
 *   featureFlagExtensionsAPI,
 *   featureFlagExtensionsInsideHybridSearch,
 *   featureFlagRankFusionFull,
 *   featureFlagSearchHybridScoringFull,
 *   featureFlagVectorSimilarityExpressions,
 *   requires_fcv_82,
 * ]
 */
import {orderedArrayEq} from "jstests/aggregation/extras/utils.js";
import {before, describe, it} from "jstests/libs/mochalite.js";

describe("extension stages inside hybrid search input pipelines", function () {
    const coll = db[jsTestName()];

    // $matchTopN: {filter, sort, limit} expands to [$match, $sort, $limit] — all selection stages.
    const matchTopNStage = {$matchTopN: {filter: {x: {$gt: 2}}, sort: {x: -1}, limit: 3}};
    const matchTopNExpanded = [{$match: {x: {$gt: 2}}}, {$sort: {x: -1}}, {$limit: 3}];

    // $addFieldsMatch expands to [$addFields, $match]; $addFields is not a selection stage.
    const addFieldsMatchStage = {
        $addFieldsMatch: {
            field: "computed",
            value: {$multiply: ["$x", 2]},
            filter: {$gt: ["$x", 2]},
        },
    };

    // $nativeVectorSearch's expansion contains $setMetadata (non-selection) after its first stage,
    // so it must be rejected even though its $sort satisfies the ranked-pipeline requirement.
    const nativeVectorSearchStage = {
        $nativeVectorSearch: {
            path: "embedding",
            queryVector: [1.0, 0.0],
            limit: 3,
            metric: "cosine",
        },
    };

    before(function () {
        coll.drop();
        assert.commandWorked(
            coll.insertMany([
                {_id: 1, x: 1, y: 50, embedding: [1.0, 0.0]},
                {_id: 2, x: 5, y: 40, embedding: [-1.0, 0.0]},
                {_id: 3, x: 3, y: 30, embedding: [0.7, 0.2]},
                {_id: 4, x: 8, y: 20, embedding: [0.8, 0.1]},
                {_id: 5, x: 2, y: 10, embedding: [0.3, 0.95]},
                {_id: 6, x: 6, y: 60, embedding: [0.1, 0.9]},
            ]),
        );
    });

    function assertExtensionMatchesExpanded(makeStage) {
        const withExtension = coll.aggregate([makeStage([matchTopNStage])]).toArray();
        const withExpanded = coll.aggregate([makeStage(matchTopNExpanded)]).toArray();
        assert(
            orderedArrayEq(withExtension, withExpanded),
            "extension result differs from its manual expansion",
            {
                withExtension,
                withExpanded,
            },
        );
        assert.gt(withExpanded.length, 0, "expected some documents from the hybrid search");
    }

    function assertRejectedAsNonSelection(pipeline, code, stageName) {
        const res = coll.runCommand("aggregate", {pipeline, cursor: {}});
        assert.commandFailedWithCode(res, code);
        assert(new RegExp(`\\${stageName}`).test(res.errmsg), "error should name the stage", {
            errmsg: res.errmsg,
        });
    }

    it("allows a selection extension in a $rankFusion input pipeline", function () {
        assertExtensionMatchesExpanded((prefix) => ({
            $rankFusion: {input: {pipelines: {a: [...prefix], b: [{$sort: {y: 1}}]}}},
        }));
    });

    it("allows a selection extension in a $scoreFusion input pipeline", function () {
        assertExtensionMatchesExpanded((prefix) => ({
            $scoreFusion: {
                input: {
                    pipelines: {
                        a: [...prefix, {$score: {score: "$x", normalization: "minMaxScaler"}}],
                        b: [{$score: {score: "$y", normalization: "minMaxScaler"}}],
                    },
                    normalization: "none",
                },
                combination: {method: "avg"},
            },
        }));
    });

    it("rejects a transforming extension in a $rankFusion input pipeline", function () {
        assertRejectedAsNonSelection(
            [
                {
                    $rankFusion: {
                        input: {
                            pipelines: {
                                a: [addFieldsMatchStage, {$sort: {x: -1}}],
                                b: [{$sort: {y: 1}}],
                            },
                        },
                    },
                },
            ],
            12108704,
            "$addFieldsMatch",
        );
    });

    it("rejects a transforming extension in a $scoreFusion input pipeline", function () {
        assertRejectedAsNonSelection(
            [
                {
                    $scoreFusion: {
                        input: {
                            pipelines: {
                                a: [
                                    addFieldsMatchStage,
                                    {$score: {score: "$x", normalization: "minMaxScaler"}},
                                ],
                                b: [{$score: {score: "$y", normalization: "minMaxScaler"}}],
                            },
                            normalization: "none",
                        },
                        combination: {method: "avg"},
                    },
                },
            ],
            12108713,
            "$addFieldsMatch",
        );
    });

    it("rejects a multi-stage extension whose later expanded stage is non-selection", function () {
        assertRejectedAsNonSelection(
            [
                {
                    $rankFusion: {
                        input: {pipelines: {a: [nativeVectorSearchStage], b: [{$sort: {y: 1}}]}},
                    },
                },
            ],
            12108704,
            "$nativeVectorSearch",
        );
    });

    it("rejects a multi-stage non-selection extension in a $scoreFusion input pipeline", function () {
        assertRejectedAsNonSelection(
            [
                {
                    $scoreFusion: {
                        input: {
                            pipelines: {
                                a: [
                                    nativeVectorSearchStage,
                                    {$score: {score: "$x", normalization: "minMaxScaler"}},
                                ],
                                b: [{$score: {score: "$y", normalization: "minMaxScaler"}}],
                            },
                            normalization: "none",
                        },
                        combination: {method: "avg"},
                    },
                },
            ],
            12108713,
            "$nativeVectorSearch",
        );
    });

    it("allows selection extensions in all $scoreFusion input pipelines simultaneously", function () {
        const res = coll
            .aggregate([
                {
                    $scoreFusion: {
                        input: {
                            pipelines: {
                                a: [
                                    {
                                        $matchTopN: {
                                            filter: {x: {$gt: 2}},
                                            sort: {x: -1},
                                            limit: 3,
                                        },
                                    },
                                    {$score: {score: "$x", normalization: "minMaxScaler"}},
                                ],
                                b: [
                                    {
                                        $matchTopN: {
                                            filter: {y: {$gt: 20}},
                                            sort: {y: -1},
                                            limit: 3,
                                        },
                                    },
                                    {$score: {score: "$y", normalization: "minMaxScaler"}},
                                ],
                            },
                            normalization: "none",
                        },
                        combination: {method: "avg"},
                    },
                },
            ])
            .toArray();
        assert.gt(
            res.length,
            0,
            "expected some documents when both $scoreFusion pipelines contain $matchTopN",
            {res},
        );
    });
});
