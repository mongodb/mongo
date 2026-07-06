/**
 * Tests $scoreFusion with extension stages on a sharded cluster.
 *
 * Exercises: $scoreFusion inside $lookup targeting a view; $matchTopN in $scoreFusion inside
 * $unionWith on a sharded view; parse-time rejection of non-selection extensions at the router.
 *
 * @tags: [
 *   featureFlagExtensionsAPI,
 *   featureFlagSearchHybridScoringFull,
 *   featureFlagVectorSimilarityExpressions,
 *   requires_sharding,
 *   requires_fcv_90,
 * ]
 */
import {assertDropCollection} from "jstests/libs/collection_drop_recreate.js";
import {before, describe, it} from "jstests/libs/mochalite.js";

describe("$scoreFusion with extension stages on a sharded cluster", function () {
    const collName = jsTestName();
    const outerName = jsTestName() + "_outer";
    const collViewName = jsTestName() + "_view";

    const matchTopNStage = {$matchTopN: {filter: {x: {$gt: 0}}, sort: {x: -1}, limit: 3}};
    const addFieldsMatchStage = {
        $addFieldsMatch: {
            field: "computed",
            value: {$multiply: ["$x", 2]},
            filter: {$gt: ["$x", 2]},
        },
    };
    const nativeVectorSearchStage = {
        $nativeVectorSearch: {
            path: "embedding",
            queryVector: [1.0, 0.0],
            limit: 3,
            metric: "cosine",
        },
    };
    const scoreStageX = {$score: {score: "$x", normalization: "minMaxScaler"}};
    const scoreStageY = {$score: {score: "$y", normalization: "minMaxScaler"}};

    before(function () {
        assertDropCollection(db, collName);
        assertDropCollection(db, outerName);
        assertDropCollection(db, collViewName);

        assert.commandWorked(
            db[collName].insertMany([
                {x: 1, y: 10},
                {x: 2, y: 20},
                {x: 3, y: 30},
            ]),
        );

        assert.commandWorked(db[outerName].insertMany([{key: "a"}, {key: "b"}]));

        assert.commandWorked(
            db.createView(collViewName, collName, [{$addFields: {fromView: true}}]),
        );
    });

    function assertRejectedAtParseTime(pipeline, code, stageName) {
        const res = db.runCommand({aggregate: collName, pipeline, cursor: {}});
        assert.commandFailedWithCode(res, code, {pipeline});
        assert(res.errmsg.includes(stageName), "error should name the rejected stage", {
            errmsg: res.errmsg,
        });
        assert(
            !/(shard\d|rs\d)/.test(res.errmsg),
            "rejection must occur at parse time at the router, not forwarded from a shard",
            {errmsg: res.errmsg},
        );
    }

    it("$scoreFusion with $matchTopN is allowed inside $lookup targeting a view on a sharded cluster", function () {
        const res = db[outerName]
            .aggregate([
                {
                    $lookup: {
                        from: collViewName,
                        pipeline: [
                            {
                                $scoreFusion: {
                                    input: {
                                        pipelines: {
                                            a: [matchTopNStage, scoreStageX],
                                            b: [scoreStageY],
                                        },
                                        normalization: "none",
                                    },
                                    combination: {method: "avg"},
                                },
                            },
                        ],
                        as: "joined",
                    },
                },
            ])
            .toArray();
        assert.eq(res.length, 2, "outer collection should return 2 docs", {res});
        assert.gt(res[0].joined.length, 0, "$scoreFusion inside $lookup should return results", {
            res,
        });
    });

    it("$matchTopN in $scoreFusion inside $unionWith against a view is allowed on a sharded cluster", function () {
        const res = db[collName]
            .aggregate([
                {
                    $unionWith: {
                        coll: collViewName,
                        pipeline: [
                            {
                                $scoreFusion: {
                                    input: {
                                        pipelines: {
                                            a: [matchTopNStage, scoreStageX],
                                            b: [scoreStageY],
                                        },
                                        normalization: "none",
                                    },
                                    combination: {method: "avg"},
                                },
                            },
                        ],
                    },
                },
            ])
            .toArray();
        assert.gt(res.length, 0, "should return documents from $unionWith", {res});
    });

    it("$addFieldsMatch in a $scoreFusion input pipeline is rejected at parse time at the router", function () {
        assertRejectedAtParseTime(
            [
                {
                    $scoreFusion: {
                        input: {
                            pipelines: {
                                a: [addFieldsMatchStage, scoreStageX],
                                b: [scoreStageY],
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

    it("$nativeVectorSearch in a $scoreFusion input pipeline is rejected at parse time at the router", function () {
        assertRejectedAtParseTime(
            [
                {
                    $scoreFusion: {
                        input: {
                            pipelines: {
                                a: [nativeVectorSearchStage, scoreStageX],
                                b: [scoreStageY],
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
});
