/**
 * Test the functionality of $rankFusion running inside the sub-pipeline of a $unionWith/$lookup,
 * when a view is involved in the query.
 *
 * This includes when the view is at the top-level of the query, and/or in the $unionWith/$lookup.
 *
 * @tags: [featureFlagSearchHybridScoringFull, requires_fcv_82]
 */

import {createSearchIndex, dropSearchIndex} from "jstests/libs/search.js";
import {
    assertDocArrExpectedFuzzy,
} from "jstests/with_mongot/e2e_lib/search_e2e_utils.js";

const collName = jsTestName();
const coll = db.getCollection(collName);
coll.drop();
assert.commandWorked(db.createCollection(coll.getName()));

const nDocs = 50;
let bulk = coll.initializeOrderedBulkOp();
for (let i = 0; i < nDocs; i++) {
    if (i % 2 === 0) {
        bulk.insert({_id: i, a: "foo", b: "apple", x: i / 3, y: i / 2});
    } else {
        bulk.insert({_id: i, a: "bar", b: "orange", x: i / 2, y: i / 3});
    }
}
assert.commandWorked(bulk.execute());

const searchIndexName = "searchIndex";
createSearchIndex(coll, {name: searchIndexName, definition: {"mappings": {"dynamic": true}}});

function buildRankFusionPipeline(inputPipelines) {
    return [{$rankFusion: {input: {pipelines: inputPipelines}}}];
}

// Builds a unionWith pipeline that runs the $rankFusion inside the $unionWith, where,
// regardless of the passed in $rankFusion, returns the results results of that $rankFusion.
// So the $unionWith acts only as a passthrough.
// In doing so, we can ensure $rankFusion works inside a $unionWith,
// in the same way as a top-level $rankFusion query.
function buildUnionPassthroughWithPipeline(viewName, rankFusionPipeline) {
    return [
        {
            // Intentionally matches nothing, so only output results are from the $unionWith.
            $match: {a: "baz"},
        },
        {$unionWith: {coll: viewName, pipeline: rankFusionPipeline}}
    ];
}

// Builds a lookup pipeline that runs the $rankFusion inside the $lookup, where,
// regardless of the passed in $rankFusion, returns the results results of that $rankFusion.
// So the $lookup acts only as a passthrough.
// In doing so, we can ensure $rankFusion works inside a $lookup,
// in the same way as a top-level $rankFusion query.
function buildLookupPassthroughPipeline(ns, rankFusionPipeline) {
    return [
        {
            $limit: 1,
        },
        {$lookup: {from: ns, as: "matched_docs", pipeline: rankFusionPipeline}},
        {$unwind: "$matched_docs"},
        {$replaceRoot: {newRoot: "$matched_docs"}}
    ];
}

(function testRankFusionInUnionWithLookupSubViewTest() {
    // Tests when the query is running on the underlying collection at the top-level,
    // and a view on the $unionWith/$lookup. Test queries like:
    // db.coll.aggregate([{$unionWith/$lookup: { from: "view", pipeline: [{$rankFusion}] }}])
    function runRankFusionInUnionWithLookupSubViewTest(testName, viewPipeline, inputPipelines) {
        const viewName = jsTestName() + "sub_view_" + testName + "_view";
        assert.commandWorked(db.createView(viewName, coll.getName(), viewPipeline));
        const view = db[viewName];

        const rankFusionPipeline = buildRankFusionPipeline(inputPipelines);
        const unionWithPipeline = buildUnionPassthroughWithPipeline(viewName, rankFusionPipeline);
        const lookupPipeline = buildLookupPassthroughPipeline(viewName, rankFusionPipeline);

        // Results of running the $rankFusion directly
        const expectedResults = view.aggregate(rankFusionPipeline);
        // Results of $rankFusion running through $unionWith passthrough.
        const unionWithResults = coll.aggregate(unionWithPipeline);
        // Results of $rankFusion running through $lookup passthrough.
        const lookupResults = coll.aggregate(lookupPipeline);

        // Direct $rankFusion results should be the same as results through the passthroughs.
        assertDocArrExpectedFuzzy(expectedResults.toArray(), lookupResults.toArray());
        assertDocArrExpectedFuzzy(expectedResults.toArray(), unionWithResults.toArray());

        // Explains for both passthroughs should work too.
        assert.commandWorked(coll.explain().aggregate(unionWithPipeline));
        assert.commandWorked(coll.explain().aggregate(lookupPipeline));
    }

    (function testMatchView() {
        runRankFusionInUnionWithLookupSubViewTest(
            "match_view_match_pipelines", [{$match: {y: {$gt: 10}}}], {
                a: [{$match: {x: {$gte: 3}}}, {$sort: {x: 1}}, {$limit: 10}],
                b: [{$match: {x: {$lte: 13}}}, {$sort: {x: -1}}, {$limit: 8}]
            });

        runRankFusionInUnionWithLookupSubViewTest(
            "match_view_search_pipeline_second", [{$match: {y: {$gt: 10}}}], {
                a: [{$search: {index: searchIndexName, text: {query: "foo", path: "a"}}}],
                b: [{$sort: {x: 1}}]
            });

        runRankFusionInUnionWithLookupSubViewTest(
            "match_pipeline_search_pipeline_second", [{$match: {y: {$gt: 10}}}], {
                a: [{$sort: {x: 1}}],
                b: [{$search: {index: searchIndexName, text: {query: "foo", path: "a"}}}]
            });

        runRankFusionInUnionWithLookupSubViewTest(
            "match_pipeline_both_search_pipelines", [{$match: {y: {$gt: 10}}}], {
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

        runRankFusionInUnionWithLookupSubViewTest(
            "search_view_search_pipeline_second",
            [{$search: {index: searchIndexName, text: {query: "apple", path: "b"}}}],
            {
                a: [{$match: {x: {$gte: 3}}}, {$sort: {x: 1}}, {$limit: 10}],
                b: [{$search: {index: searchIndexName, text: {query: "foo", path: "a"}}}]
            });

        // TODO SERVER-107803: Add search view with search first input pipeline test.
        // Note this is a search-on-views w/ $lookup general bug, not hybrid search specific.
        // Currently a $lookup on a search view with a mongot sub-pipeline asserts instead of
        // returning no results, like in a top-level query.
    })();
})();

(function testRankFusionInUnionWithLookupTopLevelViewTest() {
    // Tests when the query is running on the underlying collection at the top-level,
    // and a view on the $unionWith/$lookup. Test queries like:
    // db.view.aggregate([{$unionWith/$lookup: { from: "coll", pipeline: [{$rankFusion}] }}])
    function runRankFusionInUnionWithLookupTopLevelViewTest(
        testName, viewPipeline, inputPipelines) {
        const viewName = jsTestName() + "top_level_view_" + testName + "_view";
        assert.commandWorked(db.createView(viewName, coll.getName(), viewPipeline));
        const view = db[viewName];

        const rankFusionPipeline = buildRankFusionPipeline(inputPipelines);
        const unionWithPipeline = buildUnionPassthroughWithPipeline(collName, rankFusionPipeline);
        const lookupPipeline = buildLookupPassthroughPipeline(collName, rankFusionPipeline);

        // Test $unionWith
        const expectedUnionWithResults = coll.aggregate([...viewPipeline, ...unionWithPipeline]);
        const unionWithResults = view.aggregate(unionWithPipeline);
        assertDocArrExpectedFuzzy(expectedUnionWithResults.toArray(), unionWithResults.toArray());

        // Test $lookup
        const expectedLookupResults = coll.aggregate([...viewPipeline, ...lookupPipeline]);
        const lookupResults = view.aggregate(lookupPipeline);
        assertDocArrExpectedFuzzy(expectedLookupResults.toArray(), lookupResults.toArray());

        // Explains for both passthroughs should work too.
        assert.commandWorked(view.explain().aggregate(unionWithPipeline));
        assert.commandWorked(view.explain().aggregate(lookupPipeline));
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
        const topLevelViewName = jsTestName() + "both_views_" + testName + "_view_top_level";
        assert.commandWorked(db.createView(topLevelViewName, coll.getName(), topLevelViewPipeline));
        const topLevelView = db[topLevelViewName];

        const subViewName = jsTestName() + "both_views_" + testName + "_view_sub";
        assert.commandWorked(db.createView(subViewName, coll.getName(), subViewPipeline));
        const subView = db[subViewName];

        const rankFusionPipeline = buildRankFusionPipeline(inputPipelines);
        const unionWithPipeline =
            buildUnionPassthroughWithPipeline(subViewName, rankFusionPipeline);
        const lookupPipeline = buildLookupPassthroughPipeline(subViewName, rankFusionPipeline);

        // Test $unionWith
        const expectedUnionWithResults =
            coll.aggregate([...topLevelViewPipeline, ...unionWithPipeline]);
        const unionWithResults = topLevelView.aggregate(unionWithPipeline);
        assertDocArrExpectedFuzzy(expectedUnionWithResults.toArray(), unionWithResults.toArray());

        // Test $lookup
        const expectedLookupResults = coll.aggregate([...topLevelViewPipeline, ...lookupPipeline]);
        const lookupResults = topLevelView.aggregate(lookupPipeline);
        assertDocArrExpectedFuzzy(expectedLookupResults.toArray(), lookupResults.toArray());

        // Explains for both passthroughs should work too.
        assert.commandWorked(topLevelView.explain().aggregate(unionWithPipeline));
        assert.commandWorked(topLevelView.explain().aggregate(lookupPipeline));
    }

    (function testBothMatchViews() {
        runRankFusionInUnionWithLookupViewTopAndSubTest(
            "match_views_match_pipelines", [{$match: {y: {$gt: 10}}}], [{$match: {y: {$lt: 25}}}], {
                a: [{$match: {x: {$gte: 3}}}, {$sort: {x: 1}}, {$limit: 10}],
                b: [{$match: {x: {$lte: 13}}}, {$sort: {x: -1}}, {$limit: 8}]
            });

        runRankFusionInUnionWithLookupViewTopAndSubTest(
            "match_views_search_pipeline_first",
            [{$match: {y: {$gt: 10}}}],
            [{$match: {y: {$lt: 25}}}],
            {
                a: [{$search: {index: searchIndexName, text: {query: "foo", path: "a"}}}],
                b: [{$match: {x: {$lte: 13}}}, {$sort: {x: -1}}, {$limit: 8}]
            });

        runRankFusionInUnionWithLookupViewTopAndSubTest(
            "match_views_search_pipeline_second",
            [{$match: {y: {$gt: 10}}}],
            [{$match: {y: {$lt: 25}}}],
            {
                a: [{$match: {x: {$lte: 13}}}, {$sort: {x: -1}}, {$limit: 8}],
                b: [{$search: {index: searchIndexName, text: {query: "foo", path: "a"}}}]
            });

        runRankFusionInUnionWithLookupViewTopAndSubTest(
            "match_views_both_search_pipelines",
            [{$match: {y: {$gt: 10}}}],
            [{$match: {y: {$lt: 25}}}],
            {
                a: [{$search: {index: searchIndexName, text: {query: "bar", path: "a"}}}],
                b: [{$search: {index: searchIndexName, text: {query: "foo", path: "a"}}}]
            });
    })();

    (function testMatchTopViewSearchSubView() {
        runRankFusionInUnionWithLookupViewTopAndSubTest(
            "match_top_search_sub_view_match_pipelines",
            [{$match: {y: {$gt: 10}}}],
            [{$search: {index: searchIndexName, text: {query: "apple", path: "b"}}}],
            {
                a: [{$match: {x: {$gte: 3}}}, {$sort: {x: 1}}, {$limit: 10}],
                b: [{$match: {x: {$lte: 13}}}, {$sort: {x: -1}}, {$limit: 8}]
            });

        runRankFusionInUnionWithLookupViewTopAndSubTest(
            "match_top_search_sub_view_search_pipeline_second",
            [{$match: {y: {$gt: 10}}}],
            [{$search: {index: searchIndexName, text: {query: "apple", path: "b"}}}],
            {
                a: [{$match: {x: {$lte: 13}}}, {$sort: {x: -1}}, {$limit: 8}],
                b: [{$search: {index: searchIndexName, text: {query: "foo", path: "a"}}}],
            });

        // TODO SERVER-107803: Add search view with search first input pipeline test.
        // Note this is a search-on-views w/ $lookup general bug, not hybrid search specific.
        // Currently a $lookup on a search view with a mongot sub-pipeline asserts instead of
        // returning no results, like in a top-level query.
    })();

    (function testSearchTopViewMatchSubView() {
        runRankFusionInUnionWithLookupViewTopAndSubTest(
            "search_top_match_sub_view_match_pipelines",
            [{$search: {index: searchIndexName, text: {query: "apple", path: "b"}}}],
            [{$match: {y: {$lt: 25}}}],
            {
                a: [{$match: {x: {$gte: 3}}}, {$sort: {x: 1}}, {$limit: 10}],
                b: [{$match: {x: {$lte: 13}}}, {$sort: {x: -1}}, {$limit: 8}]
            });

        runRankFusionInUnionWithLookupViewTopAndSubTest(
            "search_top_match_sub_view_search_pipeline_first",
            [{$search: {index: searchIndexName, text: {query: "apple", path: "b"}}}],
            [{$match: {y: {$lt: 25}}}],
            {
                a: [{$search: {index: searchIndexName, text: {query: "foo", path: "a"}}}],
                b: [{$match: {x: {$lte: 13}}}, {$sort: {x: -1}}, {$limit: 8}]
            });

        runRankFusionInUnionWithLookupViewTopAndSubTest(
            "search_top_match_sub_view_search_pipeline_second",
            [{$search: {index: searchIndexName, text: {query: "apple", path: "b"}}}],
            [{$match: {y: {$lt: 25}}}],
            {
                a: [{$match: {x: {$lte: 13}}}, {$sort: {x: -1}}, {$limit: 8}],
                b: [{$search: {index: searchIndexName, text: {query: "foo", path: "a"}}}]
            });

        runRankFusionInUnionWithLookupViewTopAndSubTest(
            "search_top_match_sub_view_both_search_pipelines",
            [{$search: {index: searchIndexName, text: {query: "apple", path: "b"}}}],
            [{$match: {y: {$lt: 25}}}],
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

        runRankFusionInUnionWithLookupViewTopAndSubTest(
            "search_views_search_pipeline_second",
            [{$search: {index: searchIndexName, text: {query: "orange", path: "b"}}}],
            [{$search: {index: searchIndexName, text: {query: "apple", path: "b"}}}],
            {
                a: [{$match: {x: {$lte: 13}}}, {$sort: {x: -1}}, {$limit: 8}],
                b: [{$search: {index: searchIndexName, text: {query: "foo", path: "a"}}}],
            });

        // TODO SERVER-107803: Add search view with search first input pipeline test.
        // Note this is a search-on-views w/ $lookup general bug, not hybrid search specific.
        // Currently a $lookup on a search view with a mongot sub-pipeline asserts instead of
        // returning no results, like in a top-level query.
    })();
})();

dropSearchIndex(coll, {name: searchIndexName});
