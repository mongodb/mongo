/**
 * Tests that language features introduced in version 5.1 are not included in API Version 1
 * yet. This test should be updated or removed in a future release when we have more confidence that
 * the behavior and syntax is stable.
 *
 * @tags: [
 *   requires_fcv_51,
 *   uses_api_parameters,
 * ]
 */

(function() {
"use strict";
load("jstests/libs/api_version_helpers.js");  // For 'APIVersionHelpers'.

const collName = "api_version_new_51_language_features";
const coll = db[collName];
coll.drop();
assert.commandWorked(coll.insert({a: 1, date: new ISODate()}));

const unstablePipelines = [
    [{$set: {x: {$tsSecond: new Timestamp(0, 0)}}}],
    [{$set: {x: {$tsIncrement: new Timestamp(0, 0)}}}],
];

for (let pipeline of unstablePipelines) {
    // Assert error thrown when running a pipeline with stages not in API Version 1.
    APIVersionHelpers.assertAggregateFailsWithAPIStrict(
        pipeline, collName, ErrorCodes.APIStrictError);

    // Assert error thrown when creating a view on a pipeline with stages not in API Version 1.
    APIVersionHelpers.assertViewFailsWithAPIStrict(pipeline, collName);

    // Assert error is not thrown when running without apiStrict=true.
    assert.commandWorked(db.runCommand({
        aggregate: coll.getName(),
        pipeline: pipeline,
        apiVersion: "1",
        cursor: {},
    }));
}
})();
