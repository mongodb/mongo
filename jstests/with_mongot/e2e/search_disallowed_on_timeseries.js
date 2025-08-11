/**
 * Tests that all search stages cannot be used on timeseries collections.
 *
 * @tags: [requires_fcv_83, requires_timeseries]
 */

import {assertDropCollection} from "jstests/libs/collection_drop_recreate.js";

const timeFieldName = "time";
const metaFieldName = "tags";
const timeseriesCollName = jsTestName();
const tsColl = db.getCollection(timeseriesCollName);
assertDropCollection(db, timeseriesCollName);
assert.commandWorked(db.createCollection(
    timeseriesCollName, {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}));

const nDocs = 10;
const bulk = tsColl.initializeUnorderedBulkOp();
for (let i = 0; i < nDocs; i++) {
    const docToInsert = {[timeFieldName]: ISODate(), [metaFieldName]: i % 2};
    bulk.insert(docToInsert);
}
assert.commandWorked(bulk.execute());

const searchPipelines = [
    [{$search: {index: "default", text: {query: "example", path: metaFieldName}}}],
    [{$vectorSearch: {index: "default", vector: {$meta: "searchVector"}}}],
    [{$searchMeta: {index: "default", text: {query: "example", path: metaFieldName}}}],
    // TODO SERVER-103132 Add tests for $search in a $lookup.
    // TODO SERVER-103133 Add tests for $search in a $unionWith.
    // TODO SERVER-103134 Add tests for $search in a $graphLookup.
];

// TODO SERVER-108560 remove the legacy timeseries error codes (10623000 and 40602), once 9.0
// becomes last LTS.
// Search stages should fail when querying the timeseries collection directly.
searchPipelines.forEach(pipeline => {
    assert.commandFailedWithCode(tsColl.runCommand("aggregate", {pipeline: pipeline, cursor: {}}),
                                 [10557302, 10623000],
                                 `Expected failure for pipeline: ${tojson(pipeline)}`);
});

// Search stages should fail when querying a view on a timeseries collection.
const viewName = "view_" + timeseriesCollName;
assert.commandWorked(
    db.createView(viewName, timeseriesCollName, [{$match: {$expr: {$in: ["x", "$b"]}}}]));
searchPipelines.forEach(pipeline => {
    assert.commandFailedWithCode(
        db[viewName].runCommand("aggregate", {pipeline: pipeline, cursor: {}}),
        [10557302, 10623000],
        `Expected failure for pipeline: ${tojson(pipeline)}`);
});

// All queries on a timeseries collection on a view with $search in the view definition should fail.
const searchView = "searchview_" + timeseriesCollName;
assert.commandWorked(db.createView(searchView, timeseriesCollName, [
    {$search: {index: "default", text: {query: "example", path: metaFieldName}}}
]));
assert.commandFailedWithCode(
    db[searchView].runCommand("aggregate", {pipeline: [{$match: {}}], cursor: {}}),
    [10557302, 10623000, 40602],
    `Expected failure for pipeline: ${tojson([{$match: {}}])}`);

// '$listSearchIndexes' should return an empty array for timeseries collections.
const results = tsColl.aggregate([{$listSearchIndexes: {}}]).toArray();
assert.eq(results, [], "Expected no search indexes for timeseries collection");

// TODO SERVER-108407 Add tests for the search index commands.
