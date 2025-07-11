/**
 * Tests that $rankFusion can't be used on a view namespace if the namespace is timeseries, or if
 * the $rankFusion query has mongot input pipelines.
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

const rankFusionPipeline = [{
    $rankFusion: {
        input: {
            pipelines: {
                a: [{$sort: {x: -1}}],
                b: [{$sort: {x: 1}}],
            }
        }
    }
}];

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

// Now test on a timeseries collection (which is modeled as a view under-the-hood).
const timeFieldName = "time";
const metaFieldName = "tags";
const timeseriesCollName = "rank_fusion_timeseries";

assert.commandWorked(db.createCollection(
    timeseriesCollName, {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}));
const tsColl = db.getCollection(timeseriesCollName);

const nDocs = 50;
const bulk = tsColl.initializeUnorderedBulkOp();
for (let i = 0; i < nDocs; i++) {
    const docToInsert = {
        time: ISODate(),
        tags: {loc: [40, 40], descr: i.toString()},
        value: i + nDocs,
    };
    bulk.insert(docToInsert);
}
assert.commandWorked(bulk.execute());

// Running $rankFusion on timeseries collection is disallowed.
assert.commandFailedWithCode(
    tsColl.runCommand("aggregate", {pipeline: rankFusionPipeline, cursor: {}}),
    ErrorCodes.OptionNotSupportedOnView);
dropSearchIndex(coll, {name: searchIndexName});
dropSearchIndex(coll, {name: vectorSearchIndexName});
