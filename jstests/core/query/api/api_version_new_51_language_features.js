/**
 * Tests that language features introduced in version 5.1 are included in API Version 1.
 *
 * @tags: [
 *   requires_fcv_60,
 *   uses_api_parameters
 * ]
 */

import {APIVersionHelpers} from "jstests/libs/api_version_helpers.js";

const testDb = db.getSiblingDB(jsTestName());
const collName = "api_version_new_51_language_features";
const viewName = collName + "_view";
const coll = testDb[collName];
coll.drop();
assert.commandWorked(coll.insert({a: 1, date: new ISODate()}));

const stablePipelines = [
    [{$set: {x: {$tsSecond: new Timestamp(0, 0)}}}],
    [{$set: {x: {$tsIncrement: new Timestamp(0, 0)}}}],
];

for (let pipeline of stablePipelines) {
    // Assert error thrown when running a pipeline with stages not in API Version 1.
    APIVersionHelpers.assertAggregateSucceedsWithAPIStrict(pipeline, collName);

    // Assert error thrown when creating a view on a pipeline with stages not in API Version 1.
    APIVersionHelpers.assertViewSucceedsWithAPIStrict(pipeline, viewName, collName);

    // Assert error is not thrown when running without apiStrict=true.
    assert.commandWorked(
        testDb.runCommand({
            aggregate: coll.getName(),
            pipeline: pipeline,
            apiVersion: "1",
            cursor: {},
        }),
    );
}
