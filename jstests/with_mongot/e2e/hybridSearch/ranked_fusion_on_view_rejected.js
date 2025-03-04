/**
 * Tests that $rankFusion on a view namespace is always rejected.
 *
 * TODO SERVER-101661 Enable $rankFusion to be run on views.
 *
 * @tags: [featureFlagRankFusionBasic, requires_fcv_81]
 */

import {createSearchIndex, dropSearchIndex} from "jstests/libs/search.js";

const collName = jsTestName();
const nonSearchViewName = jsTestName() + "_view";
const searchViewName = jsTestName() + "_search_view";
const coll = db.getCollection(collName);
coll.drop();

const nDocs = 50;
let bulk = coll.initializeUnorderedBulkOp();
for (let i = 0; i < nDocs; i++) {
    if (i % 2 === 0) {
        bulk.insert({_id: i, a: "foo", x: i / 3})
    } else {
        bulk.insert({_id: i, a: "bar", x: i / 3})
    }
}
assert.commandWorked(bulk.execute());

const searchIndexName = "searchIndex";
createSearchIndex(coll, {name: searchIndexName, definition: {"mappings": {"dynamic": true}}});

const rankFusionPipelineOneInput = [{$rankFusion: {input: {pipelines: {a: [{$sort: {x: 1}}]}}}}];
const rankFusionPipelineTwoInputs = [{
    $rankFusion: {
        input: {
            pipelines: {
                a: [{$search: {index: searchIndexName, text: {query: "fo", path: "a"}}}],
                b: [{$sort: {x: 1}}],
            }
        }
    }
}];

// Create a view with a $search stage and a view without a $search stage.
assert.commandWorked(db.createView(nonSearchViewName, coll.getName(), [{$match: {a: "foo"}}]));
assert.commandWorked(db.createView(searchViewName, coll.getName(), [{$search: {}}]));

// Running a $rankFusion over the main collection (non-view) succeeds.
assert.commandWorked(
    coll.runCommand("aggregate", {pipeline: rankFusionPipelineOneInput, cursor: {}}));
assert.commandWorked(
    coll.runCommand("aggregate", {pipeline: rankFusionPipelineTwoInputs, cursor: {}}));

// Running $rankFusion over either view should fail.
const nonSearchView = db[nonSearchViewName];
const searchView = db[searchViewName];
assert.commandFailedWithCode(
    nonSearchView.runCommand("aggregate", {pipeline: rankFusionPipelineOneInput, cursor: {}}),
    ErrorCodes.CommandNotSupportedOnView);
assert.commandFailedWithCode(
    searchView.runCommand("aggregate", {pipeline: rankFusionPipelineOneInput, cursor: {}}),
    ErrorCodes.CommandNotSupportedOnView);

// Same with a $rankFusion with two input pipelines. Running this query over the search view is a
// malformed query since the post-view resolution pipeline would have 2 search stages. Regardless,
// it should first error since $rankFusion is banned on views.
assert.commandFailedWithCode(
    nonSearchView.runCommand("aggregate", {pipeline: rankFusionPipelineTwoInputs, cursor: {}}),
    ErrorCodes.CommandNotSupportedOnView);
assert.commandFailedWithCode(
    searchView.runCommand("aggregate", {pipeline: rankFusionPipelineTwoInputs, cursor: {}}),
    ErrorCodes.CommandNotSupportedOnView);

// Now test on a timeseries collection (which is modeled as a view under-the-hood).
const timeFieldName = "time";
const metaFieldName = "tags";
const timeseriesCollName = "rank_fusion_timeseries";

assert.commandWorked(db.createCollection(
    timeseriesCollName, {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}))
const tsColl = db.getCollection(timeseriesCollName);

bulk = tsColl.initializeUnorderedBulkOp();
for (let i = 0; i < nDocs; i++) {
    const docToInsert = {
        time: ISODate(),
        tags: {loc: [40, 40], descr: i.toString()},
        value: i + nDocs,
    };
    bulk.insert(docToInsert);
}
assert.commandWorked(bulk.execute());

// Running $rankFusion on timeseries collection fails.
assert.commandFailedWithCode(
    tsColl.runCommand("aggregate", {pipeline: rankFusionPipelineOneInput, cursor: {}}),
    ErrorCodes.CommandNotSupportedOnView);
assert.commandFailedWithCode(
    tsColl.runCommand("aggregate", {pipeline: rankFusionPipelineTwoInputs, cursor: {}}),
    ErrorCodes.CommandNotSupportedOnView);

// Running a $rankFusion over the main collection (non-view) still succeeds.
assert.commandWorked(
    coll.runCommand("aggregate", {pipeline: rankFusionPipelineOneInput, cursor: {}}));
assert.commandWorked(
    coll.runCommand("aggregate", {pipeline: rankFusionPipelineTwoInputs, cursor: {}}));

dropSearchIndex(coll, {name: searchIndexName});
