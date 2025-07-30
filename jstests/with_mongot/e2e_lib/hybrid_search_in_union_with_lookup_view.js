/**
 * Provides utility files to test the functionality of hybrid search stages running inside the
 * sub-pipeline of a $unionWith/$lookup, when a view is involved in the query.
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

export const searchIndexName = "searchIndex";

// Builds a unionWith pipeline that runs the hybrid search inside the $unionWith, where,
// regardless of the passed in hybrid search on, returns the results results of that hybrid search.
// So the $unionWith acts only as a passthrough.
// In doing so, we can ensure hybrid search works inside a $unionWith,
// in the same way as a top-level hybrid search query.
function buildUnionPassthroughWithPipeline(viewName, hybridSearchPipeline) {
    return [
        {
            // Intentionally matches nothing, so only output results are from the $unionWith.
            $match: {a: "baz"},
        },
        {$unionWith: {coll: viewName, pipeline: hybridSearchPipeline}}
    ];
}

// Builds a lookup pipeline that runs the hybrid search inside the $lookup, where,
// regardless of the passed in hybrid search, returns the results results of that hybrid search.
// So the $lookup acts only as a passthrough.
// In doing so, we can ensure hybrid search works inside a $lookup,
// in the same way as a top-level hybrid search query.
function buildLookupPassthroughPipeline(ns, hybridSearchPipeline) {
    return [
        {
            $limit: 1,
        },
        {$lookup: {from: ns, as: "matched_docs", pipeline: hybridSearchPipeline}},
        {$unwind: "$matched_docs"},
        {$replaceRoot: {newRoot: "$matched_docs"}}
    ];
}

// Tests when the query is running on the underlying collection at the top-level,
// and a view on the $unionWith/$lookup. Test queries like:
// db.coll.aggregate([{$unionWith/$lookup: { from: "view", pipeline: [{$rank/scoreFusion}] }}])
export function runHybridSearchInUnionWithLookupSubViewTest(
    testName, viewPipeline, inputPipelines, createHybridSearchFn) {
    createSearchIndex(coll, {name: searchIndexName, definition: {"mappings": {"dynamic": true}}});

    const viewName = jsTestName() + "sub_view_" + testName + "_view";
    assert.commandWorked(db.createView(viewName, coll.getName(), viewPipeline));
    const view = db[viewName];

    const hybridSearchPipeline = createHybridSearchFn(inputPipelines);
    const unionWithPipeline = buildUnionPassthroughWithPipeline(viewName, hybridSearchPipeline);
    const lookupPipeline = buildLookupPassthroughPipeline(viewName, hybridSearchPipeline);

    // Results of running the hybrid search directly
    const expectedResults = view.aggregate(hybridSearchPipeline);
    // Results of hybrid search running through $unionWith passthrough.
    const unionWithResults = coll.aggregate(unionWithPipeline);
    // Results of hybrid search running through $lookup passthrough.
    const lookupResults = coll.aggregate(lookupPipeline);

    // Direct hybrid search results should be the same as results through the passthroughs.
    assertDocArrExpectedFuzzy(expectedResults.toArray(), lookupResults.toArray());
    assertDocArrExpectedFuzzy(expectedResults.toArray(), unionWithResults.toArray());

    // Explains for both passthroughs should work too.
    // TODO SERVER-108243: Uncomment this line once the $unionWith serialization bug is fixed.
    // assert.commandWorked(coll.explain().aggregate(unionWithPipeline));
    assert.commandWorked(coll.explain().aggregate(lookupPipeline));

    dropSearchIndex(coll, {name: searchIndexName});
}

// Tests when the query is running on the underlying collection at the top-level,
// and a view on the $unionWith/$lookup. Test queries like:
// db.view.aggregate([{$unionWith/$lookup: { from: "coll", pipeline: [{$rank/scoreFusion}] }}])
export function runHybridSearchInUnionWithLookupTopLevelViewTest(
    testName, viewPipeline, inputPipelines, createHybridSearchFn) {
    createSearchIndex(coll, {name: searchIndexName, definition: {"mappings": {"dynamic": true}}});

    const viewName = jsTestName() + "top_level_view_" + testName + "_view";
    assert.commandWorked(db.createView(viewName, coll.getName(), viewPipeline));
    const view = db[viewName];

    const hybridSearchPipeline = createHybridSearchFn(inputPipelines);
    const unionWithPipeline = buildUnionPassthroughWithPipeline(collName, hybridSearchPipeline);
    const lookupPipeline = buildLookupPassthroughPipeline(collName, hybridSearchPipeline);

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

    dropSearchIndex(coll, {name: searchIndexName});
}

// Tests when the query is running on a view at the top-level, and a view on the
// $unionWith/$lookup. Test queries like:
// db.topLevelView.aggregate(
//   [{$unionWith/$lookup: {from: "subView", pipeline: [{$rank/scoreFusion}] }}])
export function runHybridSearchInUnionWithLookupViewTopAndSubTest(
    testName, topLevelViewPipeline, subViewPipeline, inputPipelines, createHybridSearchFn) {
    createSearchIndex(coll, {name: searchIndexName, definition: {"mappings": {"dynamic": true}}});

    const topLevelViewName = jsTestName() + "both_views_" + testName + "_view_top_level";
    assert.commandWorked(db.createView(topLevelViewName, coll.getName(), topLevelViewPipeline));
    const topLevelView = db[topLevelViewName];

    const subViewName = jsTestName() + "both_views_" + testName + "_view_sub";
    assert.commandWorked(db.createView(subViewName, coll.getName(), subViewPipeline));
    const subView = db[subViewName];

    const hybridSearchPipeline = createHybridSearchFn(inputPipelines);
    const unionWithPipeline = buildUnionPassthroughWithPipeline(subViewName, hybridSearchPipeline);
    const lookupPipeline = buildLookupPassthroughPipeline(subViewName, hybridSearchPipeline);

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
    // TODO SERVER-108243: Uncomment this line once the $unionWith serialization bug is fixed.
    // assert.commandWorked(topLevelView.explain().aggregate(unionWithPipeline));
    assert.commandWorked(topLevelView.explain().aggregate(lookupPipeline));

    dropSearchIndex(coll, {name: searchIndexName});
}
