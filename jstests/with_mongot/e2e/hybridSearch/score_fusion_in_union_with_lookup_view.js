/**
 * Test the functionality of $scoreFusion running inside the sub-pipeline of a $unionWith/$lookup,
 * when a view is involved in the query.
 *
 * This includes when the view is at the top-level of the query, and/or in the $unionWith/$lookup.
 *
 * @tags: [featureFlagSearchHybridScoringFull, requires_fcv_82]
 */
import {
    runHybridSearchInUnionWithLookupSubViewTest,
    runHybridSearchInUnionWithLookupTopLevelViewTest,
    runHybridSearchInUnionWithLookupViewTopAndSubTest,
    searchIndexName
} from "jstests/with_mongot/e2e_lib/hybrid_search_in_union_with_lookup_view.js";

function buildScoreFusionPipeline(inputPipelines) {
    return [{
        $scoreFusion: {
            input: {pipelines: inputPipelines, normalization: "sigmoid"},
            combination: {method: "avg"}
        }
    }];
}

(function testScoreFusionInUnionWithLookupSubViewTest() {
    // Tests when the query is running on the underlying collection at the top-level,
    // and a view on the $unionWith/$lookup. Test queries like:
    // db.coll.aggregate([{$unionWith/$lookup: { from: "view", pipeline: [{$scoreFusion}] }}])
    function runScoreFusionInUnionWithLookupSubViewTest(testName, viewPipeline, inputPipelines) {
        runHybridSearchInUnionWithLookupSubViewTest(
            testName, viewPipeline, inputPipelines, buildScoreFusionPipeline);
    }

    (function testMatchView() {
        runScoreFusionInUnionWithLookupSubViewTest(
            "match_view_match_pipelines", [{$match: {$expr: {$gt: ["$y", 10]}}}], {
                a: [
                    {$match: {x: {$gte: 3}}},
                    {$sort: {x: 1}},
                    {$limit: 10},
                    {$score: {score: "$x", normalization: "minMaxScaler"}}
                ],
                b: [
                    {$match: {x: {$lte: 13}}},
                    {$sort: {x: -1}},
                    {$limit: 8},
                    {$score: {score: "$y", normalization: "sigmoid"}}
                ]
            });

        runScoreFusionInUnionWithLookupSubViewTest(
            "match_view_search_pipeline_second", [{$match: {$expr: {$gt: ["$y", 10]}}}], {
                a: [{$search: {index: searchIndexName, text: {query: "foo", path: "a"}}}],
                b: [{$sort: {x: 1}}, {$score: {score: "$y", normalization: "sigmoid"}}]
            });

        runScoreFusionInUnionWithLookupSubViewTest(
            "match_pipeline_search_pipeline_second", [{$match: {$expr: {$gt: ["$y", 10]}}}], {
                a: [{$sort: {x: 1}}, {$score: {score: "$x", normalization: "minMaxScaler"}}],
                b: [{$search: {index: searchIndexName, text: {query: "foo", path: "a"}}}]
            });

        runScoreFusionInUnionWithLookupSubViewTest(
            "match_pipeline_both_search_pipelines", [{$match: {$expr: {$gt: ["$y", 10]}}}], {
                a: [{$search: {index: searchIndexName, text: {query: "apple", path: "b"}}}],
                b: [{$search: {index: searchIndexName, text: {query: "foo", path: "a"}}}]
            });
    })();

    (function testSearchView() {
        runScoreFusionInUnionWithLookupSubViewTest(
            "search_view_match_pipelines",
            [{$search: {index: searchIndexName, text: {query: "apple", path: "b"}}}],
            {
                a: [
                    {$match: {x: {$gte: 3}}},
                    {$sort: {x: 1}},
                    {$limit: 10},
                    {$score: {score: "$x", normalization: "minMaxScaler"}}
                ],
                b: [
                    {$match: {x: {$lte: 13}}},
                    {$sort: {x: -1}},
                    {$limit: 8},
                    {$score: {score: "$y", normalization: "sigmoid"}}
                ]
            });
    })();
})();

(function testScoreFusionInUnionWithLookupTopLevelViewTest() {
    // Tests when the query is running on the underlying collection at the top-level,
    // and a view on the $unionWith/$lookup. Test queries like:
    // db.view.aggregate([{$unionWith/$lookup: { from: "coll", pipeline: [{$scoreFusion}] }}])
    function runScoreFusionInUnionWithLookupTopLevelViewTest(
        testName, viewPipeline, inputPipelines) {
        runHybridSearchInUnionWithLookupTopLevelViewTest(
            testName, viewPipeline, inputPipelines, buildScoreFusionPipeline);
    }

    (function testMatchView() {
        runScoreFusionInUnionWithLookupTopLevelViewTest(
            "match_view_match_pipelines", [{$match: {$expr: {$gt: ["$y", 10]}}}], {
                a: [
                    {$match: {x: {$gte: 3}}},
                    {$sort: {x: 1}},
                    {$limit: 10},
                    {$score: {score: "$x", normalization: "minMaxScaler"}}
                ],
                b: [
                    {$match: {x: {$lte: 13}}},
                    {$sort: {x: -1}},
                    {$limit: 8},
                    {$score: {score: "$y", normalization: "sigmoid"}}
                ]
            });

        runScoreFusionInUnionWithLookupTopLevelViewTest(
            "match_view_search_pipeline_second", [{$match: {$expr: {$gt: ["$y", 10]}}}], {
                a: [{$search: {index: searchIndexName, text: {query: "foo", path: "a"}}}],
                b: [{$sort: {x: 1}}, {$score: {score: "$y", normalization: "sigmoid"}}]
            });

        runScoreFusionInUnionWithLookupTopLevelViewTest(
            "match_pipeline_search_pipeline_second", [{$match: {$expr: {$gt: ["$y", 10]}}}], {
                a: [{$sort: {x: 1}}, {$score: {score: "$x", normalization: "minMaxScaler"}}],
                b: [{$search: {index: searchIndexName, text: {query: "foo", path: "a"}}}]
            });

        runScoreFusionInUnionWithLookupTopLevelViewTest(
            "match_pipeline_both_search_pipelines", [{$match: {$expr: {$gt: ["$y", 10]}}}], {
                a: [{$search: {index: searchIndexName, text: {query: "apple", path: "b"}}}],
                b: [{$search: {index: searchIndexName, text: {query: "foo", path: "a"}}}]
            });
    })();

    (function testSearchView() {
        runScoreFusionInUnionWithLookupTopLevelViewTest(
            "search_view_match_pipelines",
            [{$search: {index: searchIndexName, text: {query: "apple", path: "b"}}}],
            {
                a: [
                    {$match: {x: {$gte: 3}}},
                    {$sort: {x: 1}},
                    {$limit: 10},
                    {$score: {score: "$x", normalization: "minMaxScaler"}}
                ],
                b: [
                    {$match: {x: {$lte: 13}}},
                    {$sort: {x: -1}},
                    {$limit: 8},
                    {$score: {score: "$y", normalization: "sigmoid"}}
                ]
            });

        runScoreFusionInUnionWithLookupTopLevelViewTest(
            "search_view_search_pipeline_first",
            [{$search: {index: searchIndexName, text: {query: "apple", path: "b"}}}],
            {
                a: [{$search: {index: searchIndexName, text: {query: "foo", path: "a"}}}],
                b: [
                    {$match: {x: {$gte: 3}}},
                    {$sort: {x: 1}},
                    {$limit: 10},
                    {$score: {score: "$y", normalization: "sigmoid"}}
                ],
            });

        runScoreFusionInUnionWithLookupTopLevelViewTest(
            "search_view_search_pipeline_second",
            [{$search: {index: searchIndexName, text: {query: "apple", path: "b"}}}],
            {
                a: [
                    {$match: {x: {$gte: 3}}},
                    {$sort: {x: 1}},
                    {$limit: 10},
                    {$score: {score: "$x", normalization: "minMaxScaler"}}
                ],
                b: [{$search: {index: searchIndexName, text: {query: "foo", path: "a"}}}]
            });

        runScoreFusionInUnionWithLookupTopLevelViewTest(
            "search_view_both_search_pipelines",
            [{$search: {index: searchIndexName, text: {query: "apple", path: "b"}}}],
            {
                a: [{$search: {index: searchIndexName, text: {query: "bar", path: "a"}}}],
                b: [{$search: {index: searchIndexName, text: {query: "foo", path: "a"}}}]
            });
    })();
})();

(function testScoreFusionInUnionWithLookupViewTopAndSubTest() {
    // Tests when the query is running on a view at the top-level, and a view on the
    // $unionWith/$lookup. Test queries like: db.topLevelView.aggregate([{$unionWith/$lookup: {
    // from: "subView", pipeline: [{$scoreFusion}] }}]) Tests all combinations of of view1 and view2
    function runScoreFusionInUnionWithLookupViewTopAndSubTest(
        testName, topLevelViewPipeline, subViewPipeline, inputPipelines) {
        runHybridSearchInUnionWithLookupViewTopAndSubTest(testName,
                                                          topLevelViewPipeline,
                                                          subViewPipeline,
                                                          inputPipelines,
                                                          buildScoreFusionPipeline);
    }

    (function testBothMatchViews() {
        runScoreFusionInUnionWithLookupViewTopAndSubTest(
            "match_views_match_pipelines",
            [{$match: {$expr: {$gt: ["$y", 10]}}}],
            [{$match: {$expr: {$lt: ["$y", 25]}}}],
            {
                a: [
                    {$match: {x: {$gte: 3}}},
                    {$sort: {x: 1}},
                    {$limit: 10},
                    {$score: {score: "$x", normalization: "minMaxScaler"}}
                ],
                b: [
                    {$match: {x: {$lte: 13}}},
                    {$sort: {x: -1}},
                    {$limit: 8},
                    {$score: {score: "$y", normalization: "sigmoid"}}
                ]
            });

        runScoreFusionInUnionWithLookupViewTopAndSubTest(
            "match_views_search_pipeline_first",
            [{$match: {$expr: {$gt: ["$y", 10]}}}],
            [{$match: {$expr: {$lt: ["$y", 25]}}}],
            {
                a: [{$search: {index: searchIndexName, text: {query: "foo", path: "a"}}}],
                b: [
                    {$match: {x: {$lte: 13}}},
                    {$sort: {x: -1}},
                    {$limit: 8},
                    {$score: {score: "$y", normalization: "sigmoid"}}
                ]
            });

        runScoreFusionInUnionWithLookupViewTopAndSubTest(
            "match_views_search_pipeline_second",
            [{$match: {$expr: {$gt: ["$y", 10]}}}],
            [{$match: {$expr: {$lt: ["$y", 25]}}}],
            {
                a: [
                    {$match: {x: {$lte: 13}}},
                    {$sort: {x: -1}},
                    {$limit: 8},
                    {$score: {score: "$x", normalization: "minMaxScaler"}}
                ],
                b: [{$search: {index: searchIndexName, text: {query: "foo", path: "a"}}}]
            });

        runScoreFusionInUnionWithLookupViewTopAndSubTest(
            "match_views_both_search_pipelines",
            [{$match: {$expr: {$gt: ["$y", 10]}}}],
            [{$match: {$expr: {$lt: ["$y", 25]}}}],
            {
                a: [{$search: {index: searchIndexName, text: {query: "bar", path: "a"}}}],
                b: [{$search: {index: searchIndexName, text: {query: "foo", path: "a"}}}]
            });
    })();

    (function testMatchTopViewSearchSubView() {
        runScoreFusionInUnionWithLookupViewTopAndSubTest(
            "match_top_search_sub_view_match_pipelines",
            [{$match: {$expr: {$gt: ["$y", 10]}}}],
            [{$search: {index: searchIndexName, text: {query: "apple", path: "b"}}}],
            {
                a: [
                    {$match: {x: {$gte: 3}}},
                    {$sort: {x: 1}},
                    {$limit: 10},
                    {$score: {score: "$x", normalization: "minMaxScaler"}}
                ],
                b: [
                    {$match: {x: {$lte: 13}}},
                    {$sort: {x: -1}},
                    {$limit: 8},
                    {$score: {score: "$y", normalization: "sigmoid"}}
                ]
            });
    })();

    (function testSearchTopViewMatchSubView() {
        runScoreFusionInUnionWithLookupViewTopAndSubTest(
            "search_top_match_sub_view_match_pipelines",
            [{$search: {index: searchIndexName, text: {query: "apple", path: "b"}}}],
            [{$match: {$expr: {$lt: ["$y", 25]}}}],
            {
                a: [
                    {$match: {x: {$gte: 3}}},
                    {$sort: {x: 1}},
                    {$limit: 10},
                    {$score: {score: "$x", normalization: "minMaxScaler"}}
                ],
                b: [
                    {$match: {x: {$lte: 13}}},
                    {$sort: {x: -1}},
                    {$limit: 8},
                    {$score: {score: "$y", normalization: "sigmoid"}}
                ]
            });

        runScoreFusionInUnionWithLookupViewTopAndSubTest(
            "search_top_match_sub_view_search_pipeline_first",
            [{$search: {index: searchIndexName, text: {query: "apple", path: "b"}}}],
            [{$match: {$expr: {$lt: ["$y", 25]}}}],
            {
                a: [{$search: {index: searchIndexName, text: {query: "foo", path: "a"}}}],
                b: [
                    {$match: {x: {$lte: 13}}},
                    {$sort: {x: -1}},
                    {$limit: 8},
                    {$score: {score: "$y", normalization: "sigmoid"}}
                ]
            });

        runScoreFusionInUnionWithLookupViewTopAndSubTest(
            "search_top_match_sub_view_search_pipeline_second",
            [{$search: {index: searchIndexName, text: {query: "apple", path: "b"}}}],
            [{$match: {$expr: {$lt: ["$y", 25]}}}],
            {
                a: [
                    {$match: {x: {$lte: 13}}},
                    {$sort: {x: -1}},
                    {$limit: 8},
                    {$score: {score: "$x", normalization: "minMaxScaler"}}
                ],
                b: [{$search: {index: searchIndexName, text: {query: "foo", path: "a"}}}]
            });

        runScoreFusionInUnionWithLookupViewTopAndSubTest(
            "search_top_match_sub_view_both_search_pipelines",
            [{$search: {index: searchIndexName, text: {query: "apple", path: "b"}}}],
            [{$match: {$expr: {$lt: ["$y", 25]}}}],
            {
                a: [{$search: {index: searchIndexName, text: {query: "bar", path: "a"}}}],
                b: [{$search: {index: searchIndexName, text: {query: "foo", path: "a"}}}]
            });
    })();

    (function testBothSearchViews() {
        runScoreFusionInUnionWithLookupViewTopAndSubTest(
            "search_views_match_pipelines",
            [{$search: {index: searchIndexName, text: {query: "orange", path: "b"}}}],
            [{$search: {index: searchIndexName, text: {query: "apple", path: "b"}}}],
            {
                a: [
                    {$match: {x: {$gte: 3}}},
                    {$sort: {x: 1}},
                    {$limit: 10},
                    {$score: {score: "$x", normalization: "minMaxScaler"}}
                ],
                b: [
                    {$match: {x: {$lte: 13}}},
                    {$sort: {x: -1}},
                    {$limit: 8},
                    {$score: {score: "$y", normalization: "sigmoid"}}
                ]
            });
    })();
})();
