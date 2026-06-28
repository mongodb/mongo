/**
 * Tests $rankFusion with extension stages against views, $lookup, $unionWith, and view chains on
 * an unsharded standalone.
 *
 * @tags: [
 *   featureFlagExtensionsAPI,
 *   featureFlagExtensionsInsideHybridSearch,
 *   featureFlagRankFusionFull,
 *   featureFlagVectorSimilarityExpressions,
 * ]
 */
import {before, describe, it} from "jstests/libs/mochalite.js";

describe("$rankFusion with extension stages on views", function () {
    const collName = jsTestName();
    const outerName = jsTestName() + "_outer";
    const collViewName = jsTestName() + "_view";
    const level1ViewName = jsTestName() + "_level1";
    const level2ViewName = jsTestName() + "_level2";

    const matchTopNStage = {$matchTopN: {filter: {x: {$gt: 2}}, sort: {x: -1}, limit: 3}};
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

    // Plain ranked-pipeline $rankFusion used by tests that only exercise view resolution.
    const plainRankFusion = {
        $rankFusion: {input: {pipelines: {a: [{$sort: {x: -1}}], b: [{$sort: {y: -1}}]}}},
    };

    before(function () {
        db.runCommand({drop: collViewName});
        db.runCommand({drop: level1ViewName});
        db.runCommand({drop: level2ViewName});
        db[collName].drop();
        db[outerName].drop();

        assert.commandWorked(
            db[collName].insertMany([
                {_id: 0, x: 1, y: 9},
                {_id: 1, x: 5, y: 4},
                {_id: 2, x: 3, y: 7},
                {_id: 3, x: 8, y: 2},
            ]),
        );
        assert.commandWorked(db[outerName].insertMany([{_id: 0}]));

        // collView passes all 4 docs (x >= 0 always holds).
        assert.commandWorked(db.createView(collViewName, collName, [{$match: {x: {$gte: 0}}}]));
        // level1View: x >= 1 (all 4 docs pass since x ∈ {1, 5, 3, 8}).
        assert.commandWorked(db.createView(level1ViewName, collName, [{$match: {x: {$gte: 1}}}]));
        // level2View: adds fromLevel2:true to every doc from level1View.
        assert.commandWorked(
            db.createView(level2ViewName, level1ViewName, [{$addFields: {fromLevel2: true}}]),
        );
    });

    function assertRejected(ns, pipeline, code, stageName) {
        const res = db.runCommand({aggregate: ns, pipeline, cursor: {}});
        assert.commandFailedWithCode(res, code);
        assert(res.errmsg.includes(stageName), "error should name the rejected stage", {
            errmsg: res.errmsg,
        });
    }

    it("$rankFusion against an unsharded view returns all documents satisfying the view filter", function () {
        const res = assert.commandWorked(
            db.runCommand({aggregate: collViewName, pipeline: [plainRankFusion], cursor: {}}),
        );
        assert.eq(4, res.cursor.firstBatch.length, "expected all 4 docs from collView", {res});
    });

    it("$rankFusion inside a $lookup subpipeline targeting a view returns joined results", function () {
        const pipeline = [
            {$lookup: {from: collViewName, as: "ranked", pipeline: [plainRankFusion]}},
        ];
        const res = assert.commandWorked(
            db.runCommand({aggregate: outerName, pipeline, cursor: {}}),
        );
        assert.eq(1, res.cursor.firstBatch.length, "one outer doc expected", {res});
        assert.eq(
            4,
            res.cursor.firstBatch[0].ranked.length,
            "ranked array should contain all 4 docs from collView",
            {res},
        );
    });

    it("$matchTopN in a $rankFusion input pipeline against a view is allowed", function () {
        const pipeline = [
            {$rankFusion: {input: {pipelines: {a: [matchTopNStage], b: [{$sort: {y: -1}}]}}}},
        ];
        const res = assert.commandWorked(
            db.runCommand({aggregate: collViewName, pipeline, cursor: {}}),
        );
        assert.gt(res.cursor.firstBatch.length, 0, "expected results from $rankFusion on view", {
            res,
        });
    });

    it("$addFieldsMatch in a $rankFusion input pipeline against a view is rejected", function () {
        const pipeline = [
            {
                $rankFusion: {
                    input: {
                        pipelines: {
                            a: [addFieldsMatchStage, {$sort: {x: -1}}],
                            b: [{$sort: {y: -1}}],
                        },
                    },
                },
            },
        ];
        assertRejected(collViewName, pipeline, 12108704, "$addFieldsMatch");
    });

    it("$matchTopN in $rankFusion inside a $unionWith targeting a view succeeds", function () {
        const pipeline = [
            {
                $unionWith: {
                    coll: collViewName,
                    pipeline: [
                        {
                            $rankFusion: {
                                input: {pipelines: {a: [matchTopNStage], b: [{$sort: {y: -1}}]}},
                            },
                        },
                    ],
                },
            },
        ];
        const res = assert.commandWorked(
            db.runCommand({aggregate: outerName, pipeline, cursor: {}}),
        );
        // The outer collection contributes 1 doc; $rankFusion against collView adds more.
        // Pipeline b ranks all 4 view docs, so the union result is 1 + 4 = 5.
        assert.eq(
            5,
            res.cursor.firstBatch.length,
            "expected 1 outer doc plus 4 docs from $rankFusion on collView",
            {res},
        );
    });

    it("$rankFusion on a two-level view chain applies all view transforms", function () {
        const res = assert.commandWorked(
            db.runCommand({aggregate: level2ViewName, pipeline: [plainRankFusion], cursor: {}}),
        );
        // level1View passes all 4 docs (x ∈ {1,5,3,8}, all >= 1).
        // level2View adds fromLevel2:true to each.
        assert.eq(4, res.cursor.firstBatch.length, "expected all 4 docs from the view chain", {
            res,
        });
        assert(
            res.cursor.firstBatch.every((doc) => doc.fromLevel2 === true),
            "every doc must have fromLevel2:true from level2View",
            {res},
        );
    });

    it("$nativeVectorSearch in a $rankFusion input pipeline against a view is rejected", function () {
        const pipeline = [
            {
                $rankFusion: {
                    input: {pipelines: {a: [nativeVectorSearchStage], b: [{$sort: {y: -1}}]}},
                },
            },
        ];
        assertRejected(collViewName, pipeline, 12108704, "$nativeVectorSearch");
    });
});
