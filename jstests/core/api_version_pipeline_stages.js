/**
 * Tests commands(e.g. aggregate, create) that use pipeline stages support API Version 1.
 *
 * Tests which create views aren't expected to work when collections are implicitly sharded.
 * @tags: [
 *   requires_fcv_49,
 *   uses_api_parameters,
 *   assumes_unsharded_collection,
 *   assumes_read_preference_unchanged,
 *   assumes_read_concern_unchanged
 * ]
 */

(function() {
"use strict";

const collName = "api_version_pipeline_stages";
const coll = db[collName];
coll.drop();
coll.insert({a: 1});

const pipelines = [
    [{$collStats: {count: {}}}],
    [{$currentOp: {}}],
    [{$indexStats: {}}],
    [{$listLocalSessions: {}}],
    [{$listSessions: {}}],
    [{$planCacheStats: {}}],
    [{$unionWith: {coll: "coll2", pipeline: [{$collStats: {count: {}}}]}}],
    [{$lookup: {from: "coll2", pipeline: [{$indexStats: {}}]}}],
    [{$facet: {field1: [], field2: [{$indexStats: {}}]}}],
];

for (let pipeline of pipelines) {
    // Assert error thrown when running a pipeline with stages not in API Version 1.
    assert.commandFailedWithCode(db.runCommand({
        aggregate: collName,
        pipeline: pipeline,
        cursor: {},
        apiStrict: true,
        apiVersion: "1"
    }),
                                 ErrorCodes.APIStrictError);

    // Assert error thrown when creating a view on a pipeline with stages not in API Version 1.
    assert.commandFailedWithCode(db.runCommand({
        create: 'api_version_pipeline_stages_should_fail',
        viewOn: collName,
        pipeline: pipeline,
        apiStrict: true,
        apiVersion: "1"
    }),
                                 ErrorCodes.APIStrictError);
}
})();
