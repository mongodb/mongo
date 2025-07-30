/**
 * Test the functionality of $rankFusion running inside the sub-pipeline of a $unionWith/$lookup,
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

function buildRankFusionPipeline(inputPipelines) {
    return [{$rankFusion: {input: {pipelines: inputPipelines}}}];
}

(function testRankFusionInUnionWithLookupSubViewTest() {
    // Tests when the query is running on the underlying collection at the top-level,
    // and a view on the $unionWith/$lookup. Test queries like:
    // db.coll.aggregate([{$unionWith/$lookup: { from: "view", pipeline: [{$rankFusion}] }}])
    function runRankFusionInUnionWithLookupSubViewTest(testName, viewPipeline, inputPipelines) {
        runHybridSearchInUnionWithLookupSubViewTest(
            testName, viewPipeline, inputPipelines, buildRankFusionPipeline);
    }

    (function testMatchView() {
        runRankFusionInUnionWithLookupSubViewTest(
            "match_view_match_pipelines", [{$match: {$expr: {$gt: ["$y", 10]}}}], {
                a: [{$match: {x: {$gte: 3}}}, {$sort: {x: 1}}, {$limit: 10}],
                b: [{$match: {x: {$lte: 13}}}, {$sort: {x: -1}}, {$limit: 8}]
            });

        runRankFusionInUnionWithLookupSubViewTest(
            "match_view_search_pipeline_first", [{$match: {$expr: {$gt: ["$y", 10]}}}], {
                a: [{$search: {index: searchIndexName, text: {query: "foo", path: "a"}}}],
                b: [{$sort: {x: 1}}]
            });

        runRankFusionInUnionWithLookupSubViewTest(
            "match_pipeline_search_pipeline_second", [{$match: {$expr: {$gt: ["$y", 10]}}}], {
                a: [{$sort: {x: 1}}],
                b: [{$search: {index: searchIndexName, text: {query: "foo", path: "a"}}}]
            });

        runRankFusionInUnionWithLookupSubViewTest(
            "match_pipeline_both_search_pipelines", [{$match: {$expr: {$gt: ["$y", 10]}}}], {
                a: [{$search: {index: searchIndexName, text: {query: "apple", path: "b"}}}],
                b: [{$search: {index: searchIndexName, text: {query: "foo", path: "a"}}}]
            });
    })();

    (function testSearchView() {
        runRankFusionInUnionWithLookupSubViewTest(
            "search_view_match_pipelines",
            [{$search: {index: searchIndexName, text: {query: "apple", path: "b"}}}],
            {
                a: [{$match: {x: {$gte: 3}}}, {$sort: {x: 1}}, {$limit: 10}],
                b: [{$match: {x: {$lte: 13}}}, {$sort: {x: -1}}, {$limit: 8}]
            });
    })();
})();

(function testRankFusionInUnionWithLookupTopLevelViewTest() {
    // Tests when the query is running on the underlying collection at the top-level,
    // and a view on the $unionWith/$lookup. Test queries like:
    // db.view.aggregate([{$unionWith/$lookup: { from: "coll", pipeline: [{$rankFusion}] }}])
    function runRankFusionInUnionWithLookupTopLevelViewTest(
        testName, viewPipeline, inputPipelines) {
        runHybridSearchInUnionWithLookupTopLevelViewTest(
            testName, viewPipeline, inputPipelines, buildRankFusionPipeline);
    }

    (function testMatchView() {
        runRankFusionInUnionWithLookupTopLevelViewTest(
            "match_view_match_pipelines", [{$match: {y: {$gt: 10}}}], {
                a: [{$match: {x: {$gte: 3}}}, {$sort: {x: 1}}, {$limit: 10}],
                b: [{$match: {x: {$lte: 13}}}, {$sort: {x: -1}}, {$limit: 8}]
            });

        runRankFusionInUnionWithLookupTopLevelViewTest(
            "match_view_search_pipeline_second", [{$match: {y: {$gt: 10}}}], {
                a: [{$search: {index: searchIndexName, text: {query: "foo", path: "a"}}}],
                b: [{$sort: {x: 1}}]
            });

        runRankFusionInUnionWithLookupTopLevelViewTest(
            "match_pipeline_search_pipeline_second", [{$match: {y: {$gt: 10}}}], {
                a: [{$sort: {x: 1}}],
                b: [{$search: {index: searchIndexName, text: {query: "foo", path: "a"}}}]
            });

        runRankFusionInUnionWithLookupTopLevelViewTest(
            "match_pipeline_both_search_pipelines", [{$match: {y: {$gt: 10}}}], {
                a: [{$search: {index: searchIndexName, text: {query: "apple", path: "b"}}}],
                b: [{$search: {index: searchIndexName, text: {query: "foo", path: "a"}}}]
            });
    })();

    (function testSearchView() {
        runRankFusionInUnionWithLookupTopLevelViewTest(
            "search_view_match_pipelines",
            [{$search: {index: searchIndexName, text: {query: "apple", path: "b"}}}],
            {
                a: [{$match: {x: {$gte: 3}}}, {$sort: {x: 1}}, {$limit: 10}],
                b: [{$match: {x: {$lte: 13}}}, {$sort: {x: -1}}, {$limit: 8}]
            });

        runRankFusionInUnionWithLookupTopLevelViewTest(
            "search_view_search_pipeline_first",
            [{$search: {index: searchIndexName, text: {query: "apple", path: "b"}}}],
            {
                a: [{$search: {index: searchIndexName, text: {query: "foo", path: "a"}}}],
                b: [{$match: {x: {$gte: 3}}}, {$sort: {x: 1}}, {$limit: 10}],
            });

        runRankFusionInUnionWithLookupTopLevelViewTest(
            "search_view_search_pipeline_second",
            [{$search: {index: searchIndexName, text: {query: "apple", path: "b"}}}],
            {
                a: [{$match: {x: {$gte: 3}}}, {$sort: {x: 1}}, {$limit: 10}],
                b: [{$search: {index: searchIndexName, text: {query: "foo", path: "a"}}}]
            });

        runRankFusionInUnionWithLookupTopLevelViewTest(
            "search_view_both_search_pipelines",
            [{$search: {index: searchIndexName, text: {query: "apple", path: "b"}}}],
            {
                a: [{$search: {index: searchIndexName, text: {query: "bar", path: "a"}}}],
                b: [{$search: {index: searchIndexName, text: {query: "foo", path: "a"}}}]
            });
    })();
})();

(function testRankFusionInUnionWithLookupViewTopAndSubTest() {
    // Tests when the query is running on a view at the top-level, and a view on the
    // $unionWith/$lookup. Test queries like: db.topLevelView.aggregate([{$unionWith/$lookup: {
    // from: "subView", pipeline: [{$rankFusion}] }}]) Tests all combinations of of view1 and view2
    function runRankFusionInUnionWithLookupViewTopAndSubTest(
        testName, topLevelViewPipeline, subViewPipeline, inputPipelines) {
        runHybridSearchInUnionWithLookupViewTopAndSubTest(testName,
                                                          topLevelViewPipeline,
                                                          subViewPipeline,
                                                          inputPipelines,
                                                          buildRankFusionPipeline);
    }

    (function testBothMatchViews() {
        runRankFusionInUnionWithLookupViewTopAndSubTest(
            "match_views_match_pipelines",
            [{$match: {$expr: {$gt: ["$y", 10]}}}],
            [{$match: {$expr: {$lt: ["$y", 25]}}}],
            {
                a: [{$match: {x: {$gte: 3}}}, {$sort: {x: 1}}, {$limit: 10}],
                b: [{$match: {x: {$lte: 13}}}, {$sort: {x: -1}}, {$limit: 8}]
            });

        runRankFusionInUnionWithLookupViewTopAndSubTest(
            "match_views_search_pipeline_first",
            [{$match: {$expr: {$gt: ["$y", 10]}}}],
            [{$match: {$expr: {$lt: ["$y", 25]}}}],
            {
                a: [{$search: {index: searchIndexName, text: {query: "foo", path: "a"}}}],
                b: [{$match: {x: {$lte: 13}}}, {$sort: {x: -1}}, {$limit: 8}]
            });

        runRankFusionInUnionWithLookupViewTopAndSubTest(
            "match_views_search_pipeline_second",
            [{$match: {$expr: {$gt: ["$y", 10]}}}],
            [{$match: {$expr: {$lt: ["$y", 25]}}}],
            {
                a: [{$match: {x: {$lte: 13}}}, {$sort: {x: -1}}, {$limit: 8}],
                b: [{$search: {index: searchIndexName, text: {query: "foo", path: "a"}}}]
            });

        runRankFusionInUnionWithLookupViewTopAndSubTest(
            "match_views_both_search_pipelines",
            [{$match: {$expr: {$gt: ["$y", 10]}}}],
            [{$match: {$expr: {$lt: ["$y", 25]}}}],
            {
                a: [{$search: {index: searchIndexName, text: {query: "bar", path: "a"}}}],
                b: [{$search: {index: searchIndexName, text: {query: "foo", path: "a"}}}]
            });
    })();

    (function testMatchTopViewSearchSubView() {
        runRankFusionInUnionWithLookupViewTopAndSubTest(
            "match_top_search_sub_view_match_pipelines",
            [{$match: {$expr: {$gt: ["$y", 10]}}}],
            [{$search: {index: searchIndexName, text: {query: "apple", path: "b"}}}],
            {
                a: [{$match: {x: {$gte: 3}}}, {$sort: {x: 1}}, {$limit: 10}],
                b: [{$match: {x: {$lte: 13}}}, {$sort: {x: -1}}, {$limit: 8}]
            });
    })();

    (function testSearchTopViewMatchSubView() {
        runRankFusionInUnionWithLookupViewTopAndSubTest(
            "search_top_match_sub_view_match_pipelines",
            [{$search: {index: searchIndexName, text: {query: "apple", path: "b"}}}],
            [{$match: {$expr: {$lt: ["$y", 25]}}}],
            {
                a: [{$match: {x: {$gte: 3}}}, {$sort: {x: 1}}, {$limit: 10}],
                b: [{$match: {x: {$lte: 13}}}, {$sort: {x: -1}}, {$limit: 8}]
            });

        runRankFusionInUnionWithLookupViewTopAndSubTest(
            "search_top_match_sub_view_search_pipeline_first",
            [{$search: {index: searchIndexName, text: {query: "apple", path: "b"}}}],
            [{$match: {$expr: {$lt: ["$y", 25]}}}],
            {
                a: [{$search: {index: searchIndexName, text: {query: "foo", path: "a"}}}],
                b: [{$match: {x: {$lte: 13}}}, {$sort: {x: -1}}, {$limit: 8}]
            });

        runRankFusionInUnionWithLookupViewTopAndSubTest(
            "search_top_match_sub_view_search_pipeline_second",
            [{$search: {index: searchIndexName, text: {query: "apple", path: "b"}}}],
            [{$match: {$expr: {$lt: ["$y", 25]}}}],
            {
                a: [{$match: {x: {$lte: 13}}}, {$sort: {x: -1}}, {$limit: 8}],
                b: [{$search: {index: searchIndexName, text: {query: "foo", path: "a"}}}]
            });

        runRankFusionInUnionWithLookupViewTopAndSubTest(
            "search_top_match_sub_view_both_search_pipelines",
            [{$search: {index: searchIndexName, text: {query: "apple", path: "b"}}}],
            [{$match: {$expr: {$lt: ["$y", 25]}}}],
            {
                a: [{$search: {index: searchIndexName, text: {query: "bar", path: "a"}}}],
                b: [{$search: {index: searchIndexName, text: {query: "foo", path: "a"}}}]
            });
    })();

    (function testBothSearchViews() {
        runRankFusionInUnionWithLookupViewTopAndSubTest(
            "search_views_match_pipelines",
            [{$search: {index: searchIndexName, text: {query: "orange", path: "b"}}}],
            [{$search: {index: searchIndexName, text: {query: "apple", path: "b"}}}],
            {
                a: [{$match: {x: {$gte: 3}}}, {$sort: {x: 1}}, {$limit: 10}],
                b: [{$match: {x: {$lte: 13}}}, {$sort: {x: -1}}, {$limit: 8}]
            });
    })();
})();
