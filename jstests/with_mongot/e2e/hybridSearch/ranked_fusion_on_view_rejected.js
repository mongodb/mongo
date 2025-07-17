/**
 * Tests that $rankFusion can't be used on a view namespace or if the $rankFusion query has mongot
 * input pipelines. A corresponding test for timeseries collections is in
 * jstests/core/timeseries/query/timeseries_rank_fusion_disallowed.js.
 *
 * @tags: [featureFlagSearchHybridScoringFull, requires_fcv_81]
 */

import {createSearchIndex, dropSearchIndex} from "jstests/libs/search.js";

const collName = jsTestName();
const coll = db.getCollection(collName);
coll.drop();
assert.commandWorked(db.createCollection(coll.getName()));

const searchIndexName = "searchIndex";
createSearchIndex(coll, {name: searchIndexName, definition: {"mappings": {"dynamic": true}}});
const vectorSearchIndexName = "vectorSearchIndex";
createSearchIndex(coll, {
    name: vectorSearchIndexName,
    type: "vectorSearch",
    definition: {
        "fields": [{
            "type": "vector",
            "numDimensions": 1536,
            "path": "plot_embedding",
            "similarity": "euclidean"
        }]
    }
});

assert.commandWorked(coll.createIndex({loc: "2dsphere"}));

const matchViewName = collName + "_match_view";
const matchViewPipeline = {
    $match: {a: "foo"}
};

// Create a view with $match.
assert.commandWorked(db.createView(matchViewName, coll.getName(), [matchViewPipeline]));

// Cannot create a search index of the same name on both the view and underlying collection.
const matchExprViewName = collName + "_match_expr_view";
const matchExprViewPipeline = {
    $match: {$expr: {$eq: ["$a", "bar"]}}
};
assert.commandWorked(db.createView(matchExprViewName, coll.getName(), [matchExprViewPipeline]));
const matchExprView = db[matchExprViewName];
assert.throwsWithCode(
    () => createSearchIndex(matchExprView,
                            {name: searchIndexName, definition: {"mappings": {"dynamic": true}}}),
    ErrorCodes.BadValue);
assert.throwsWithCode(
    () => createSearchIndex(
        matchExprView, {name: vectorSearchIndexName, definition: {"mappings": {"dynamic": true}}}),
    ErrorCodes.BadValue);

dropSearchIndex(coll, {name: searchIndexName});
dropSearchIndex(coll, {name: vectorSearchIndexName});
