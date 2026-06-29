/**
 * Tests hybrid search stages ($rankFusion/$scoreFusion) whose input pipelines contain extension
 * stages, nested inside $unionWith and $lookup — including multi-level nesting and views.
 *
 * Builds on extension_in_hybrid_search.js, which covers the same input-pipeline validation at the
 * top level: input pipelines may only contain selection stages (judged across every stage of an
 * extension's expansion), so $matchTopN ($match + $sort + $limit) is accepted while
 * $nativeVectorSearch ($setMetadata in its expansion) is rejected.
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

import {before, describe, it} from "jstests/libs/mochalite.js";

const testDb = db.getSiblingDB(jsTestName());

// Target collection the hybrid search runs against (same dataset as extension_in_hybrid_search.js).
const target = testDb["target"];
// Outer collection for $unionWith/$lookup combos. A single doc so identity-shaped outer stages do
// not disturb the hybrid search results flowing through them.
const outer = testDb["outer"];
// Empty collection so identity-$unionWith returns exactly the subpipeline's results.
const empty = testDb["empty"];

// Extension input pipelines. $matchTopN expands to [$match, $sort, $limit] — all selection
// stages, ranked via its $sort.
//   a: x > 2, sort x desc, limit 3 -> _ids [4, 6, 2]
//   b: y < 45, sort y asc, limit 2 -> _ids [5, 4]
const matchTopNByX = {$matchTopN: {filter: {x: {$gt: 2}}, sort: {x: -1}, limit: 3}};
const matchTopNByY = {$matchTopN: {filter: {y: {$lt: 45}}, sort: {y: 1}, limit: 2}};

// Reciprocal rank fusion (k=60) over a=[4,6,2], b=[5,4]:
//   4: 1/61 + 1/62 = .032522    5: 1/61 = .016393
//   6: 1/62       = .016129    2: 1/63 = .015873
const expectedRankFusionIds = [4, 5, 6, 2];

const hybridSearchBuilders = {
    rankFusion: () => ({
        $rankFusion: {input: {pipelines: {a: [matchTopNByX], b: [matchTopNByY]}}},
    }),
    scoreFusion: () => ({
        $scoreFusion: {
            input: {
                pipelines: {
                    a: [matchTopNByX, {$score: {score: "$x", normalization: "minMaxScaler"}}],
                    b: [matchTopNByY, {$score: {score: "$y", normalization: "minMaxScaler"}}],
                },
                normalization: "none",
            },
            combination: {method: "avg"},
        },
    }),
};

function getIds(results) {
    return results.map((d) => d._id);
}

describe("extension stages inside hybrid search, nested in $unionWith/$lookup", function () {
    before(function () {
        testDb.dropDatabase();
        assert.commandWorked(
            target.insertMany([
                {_id: 1, x: 1, y: 50},
                {_id: 2, x: 5, y: 40},
                {_id: 3, x: 3, y: 30},
                {_id: 4, x: 8, y: 20},
                {_id: 5, x: 2, y: 10},
                {_id: 6, x: 6, y: 60},
            ]),
        );
        assert.commandWorked(outer.insertOne({_id: 0, isOuter: true}));
        assert.commandWorked(testDb.createCollection(empty.getName()));
    });

    for (const [name, buildHybridSearch] of Object.entries(hybridSearchBuilders)) {
        describe(name, function () {
            // Baseline: top-level hybrid search with extension input pipelines on the target
            // collection. All nested combos must reproduce these results.
            let expectedIds;
            before(function () {
                expectedIds = getIds(target.aggregate([buildHybridSearch()]).toArray());
                assert.eq(expectedIds.length, 4, expectedIds);
                if (name === "rankFusion") {
                    // Hand-computed RRF ordering — guards against the direct run and the nested
                    // runs being identically wrong.
                    assert.eq(expectedIds, expectedRankFusionIds);
                }
            });

            it("inside $unionWith", function () {
                const results = empty
                    .aggregate([
                        {$unionWith: {coll: target.getName(), pipeline: [buildHybridSearch()]}},
                    ])
                    .toArray();
                assert.eq(getIds(results), expectedIds, results);
            });

            it("inside $unionWith preserves outer documents", function () {
                const results = outer
                    .aggregate([
                        {$unionWith: {coll: target.getName(), pipeline: [buildHybridSearch()]}},
                    ])
                    .toArray();
                // The outer doc streams first, then the union'ed hybrid search results in order.
                assert.eq(getIds(results), [0].concat(expectedIds), results);
            });

            it("inside $lookup", function () {
                const results = outer
                    .aggregate([
                        {
                            $lookup: {
                                from: target.getName(),
                                pipeline: [buildHybridSearch()],
                                as: "out",
                            },
                        },
                    ])
                    .toArray();
                assert.eq(results.length, 1, results);
                assert.eq(getIds(results[0].out), expectedIds, results);
            });

            it("inside $unionWith inside $unionWith", function () {
                const results = empty
                    .aggregate([
                        {
                            $unionWith: {
                                coll: empty.getName(),
                                pipeline: [
                                    {
                                        $unionWith: {
                                            coll: target.getName(),
                                            pipeline: [buildHybridSearch()],
                                        },
                                    },
                                ],
                            },
                        },
                    ])
                    .toArray();
                assert.eq(getIds(results), expectedIds, results);
            });

            it("inside $lookup inside $unionWith", function () {
                const results = empty
                    .aggregate([
                        {
                            $unionWith: {
                                coll: outer.getName(),
                                pipeline: [
                                    {
                                        $lookup: {
                                            from: target.getName(),
                                            pipeline: [buildHybridSearch()],
                                            as: "out",
                                        },
                                    },
                                ],
                            },
                        },
                    ])
                    .toArray();
                assert.eq(results.length, 1, results);
                assert.eq(getIds(results[0].out), expectedIds, results);
            });

            it("inside $unionWith inside $lookup", function () {
                const results = outer
                    .aggregate([
                        {
                            $lookup: {
                                from: empty.getName(),
                                pipeline: [
                                    {
                                        $unionWith: {
                                            coll: target.getName(),
                                            pipeline: [buildHybridSearch()],
                                        },
                                    },
                                ],
                                as: "out",
                            },
                        },
                    ])
                    .toArray();
                assert.eq(results.length, 1, results);
                assert.eq(getIds(results[0].out), expectedIds, results);
            });
        });
    }

    describe("view combos", function () {
        // View over 'target' excluding _id 6 (x: 6). Over the view the input pipelines yield
        //   a: [4, 2, 3]    b: [5, 4]
        // fusing (RRF k=60) to [4, 5, 2, 3] — distinct from the collection-based ordering, so a
        // combo that silently ignored the view definition would be caught.
        const viewName = "targetView";
        const expectedViewRankFusionIds = [4, 5, 2, 3];
        let view;

        before(function () {
            assert.commandWorked(
                testDb.createView(viewName, target.getName(), [{$match: {x: {$ne: 6}}}]),
            );
            view = testDb[viewName];
        });

        for (const [name, buildHybridSearch] of Object.entries(hybridSearchBuilders)) {
            describe(name, function () {
                // Baselines: hybrid search with extension inputs directly on the view and on the
                // underlying collection. The direct-on-view query exercises view binding of the
                // extension input pipelines and must succeed.
                let expectedViewIds;
                let expectedCollIds;
                before(function () {
                    expectedViewIds = getIds(view.aggregate([buildHybridSearch()]).toArray());
                    assert.eq(expectedViewIds.length, 4, expectedViewIds);
                    if (name === "rankFusion") {
                        assert.eq(expectedViewIds, expectedViewRankFusionIds);
                    }
                    expectedCollIds = getIds(target.aggregate([buildHybridSearch()]).toArray());
                });

                it("view at the top level of the query", function () {
                    // The outer $match selects no view documents, so only the union'ed hybrid
                    // search results (computed over the plain collection) flow through.
                    const results = view
                        .aggregate([
                            {$match: {_id: -1}},
                            {$unionWith: {coll: target.getName(), pipeline: [buildHybridSearch()]}},
                        ])
                        .toArray();
                    assert.eq(getIds(results), expectedCollIds, results);
                });

                it("view as the $unionWith target", function () {
                    const results = empty
                        .aggregate([
                            {$unionWith: {coll: viewName, pipeline: [buildHybridSearch()]}},
                        ])
                        .toArray();
                    assert.eq(getIds(results), expectedViewIds, results);
                });

                it("views at both the top level and the $unionWith target", function () {
                    // $match selects no view docs, so only the union's hybrid-search results flow.
                    const results = view
                        .aggregate([
                            {$match: {_id: -1}},
                            {$unionWith: {coll: viewName, pipeline: [buildHybridSearch()]}},
                        ])
                        .toArray();
                    assert.eq(getIds(results), expectedViewIds, results);
                });

                it("view at the inner level of a two-level $unionWith chain", function () {
                    const results = empty
                        .aggregate([
                            {
                                $unionWith: {
                                    coll: empty.getName(),
                                    pipeline: [
                                        {
                                            $unionWith: {
                                                coll: viewName,
                                                pipeline: [buildHybridSearch()],
                                            },
                                        },
                                    ],
                                },
                            },
                        ])
                        .toArray();
                    assert.eq(getIds(results), expectedViewIds, results);
                });
            });
        }
    });

    describe("sequential hybrid-search-bearing subpipeline stages", function () {
        // Two $unionWith stages in the same top-level pipeline, each running hybrid search with
        // extension input pipelines. Results stream in stage order: first union's fused docs,
        // then the second's.
        it("two $unionWith stages with the same hybrid search stage", function () {
            for (const buildHybridSearch of Object.values(hybridSearchBuilders)) {
                const expectedIds = getIds(target.aggregate([buildHybridSearch()]).toArray());
                const results = empty
                    .aggregate([
                        {$unionWith: {coll: target.getName(), pipeline: [buildHybridSearch()]}},
                        {$unionWith: {coll: target.getName(), pipeline: [buildHybridSearch()]}},
                    ])
                    .toArray();
                assert.eq(getIds(results), expectedIds.concat(expectedIds), results);
            }
        });

        it("a $rankFusion $unionWith followed by a $scoreFusion $unionWith", function () {
            const expectedRankIds = getIds(
                target.aggregate([hybridSearchBuilders.rankFusion()]).toArray(),
            );
            const expectedScoreIds = getIds(
                target.aggregate([hybridSearchBuilders.scoreFusion()]).toArray(),
            );
            const results = empty
                .aggregate([
                    {
                        $unionWith: {
                            coll: target.getName(),
                            pipeline: [hybridSearchBuilders.rankFusion()],
                        },
                    },
                    {
                        $unionWith: {
                            coll: target.getName(),
                            pipeline: [hybridSearchBuilders.scoreFusion()],
                        },
                    },
                ])
                .toArray();
            assert.eq(getIds(results), expectedRankIds.concat(expectedScoreIds), results);
        });
    });

    describe("input-pipeline validation propagates through nesting", function () {
        // $nativeVectorSearch's expansion contains $setMetadata (non-selection), so it is rejected
        // as a hybrid search input — nesting inside $unionWith/$lookup must not bypass that.
        const rejectedHybridStage = {
            $rankFusion: {
                input: {
                    pipelines: {
                        a: [
                            {
                                $nativeVectorSearch: {
                                    path: "embedding",
                                    queryVector: [1.0, 0.0],
                                    limit: 3,
                                    metric: "cosine",
                                },
                            },
                        ],
                        b: [{$sort: {y: 1}}],
                    },
                },
            },
        };
        const kNonSelectionInRankFusion = 12108704;

        it("rejects a non-selection extension input inside $unionWith", function () {
            assert.commandFailedWithCode(
                testDb.runCommand("aggregate", {
                    aggregate: outer.getName(),
                    pipeline: [
                        {$unionWith: {coll: target.getName(), pipeline: [rejectedHybridStage]}},
                    ],
                    cursor: {},
                }),
                kNonSelectionInRankFusion,
            );
        });

        it("rejects a non-selection extension input inside $lookup", function () {
            assert.commandFailedWithCode(
                testDb.runCommand("aggregate", {
                    aggregate: outer.getName(),
                    pipeline: [
                        {
                            $lookup: {
                                from: target.getName(),
                                pipeline: [rejectedHybridStage],
                                as: "out",
                            },
                        },
                    ],
                    cursor: {},
                }),
                kNonSelectionInRankFusion,
            );
        });
    });
});
