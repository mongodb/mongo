/**
 * Tests that language features introduced in version 4.9 or 5.0 are included in API Version 1.
 *
 * @tags: [
 *   requires_fcv_60,
 *   uses_api_parameters,
 * ]
 */

(function() {
"use strict";
load("jstests/libs/api_version_helpers.js");  // For 'APIVersionHelpers'.

const collName = "api_version_new_50_language_features";
const coll = db[collName];
coll.drop();
assert.commandWorked(coll.insert({a: 1, date: new ISODate()}));

const stablePipelines = [
    [{$set: {x: {$dateTrunc: {date: "$date", unit: "second", binSize: 5}}}}],
    [{$set: {x: {$dateAdd: {startDate: "$date", unit: "day", amount: 1}}}}],
    [{$set: {x: {$dateSubtract: {startDate: "$date", unit: "day", amount: 1}}}}],
    [{$set: {x: {$dateDiff: {startDate: "$date", endDate: "$date", unit: "day"}}}}],
    [{$set: {x: {$getField: {input: "$$ROOT", field: "x"}}}}],
    [{$set: {x: {$setField: {input: "$$ROOT", field: "x", value: "foo"}}}}],
];

function assertAggregateFailsWithAPIStrict(pipeline, errorCodes) {
    assert.commandFailedWithCode(db.runCommand({
        aggregate: collName,
        pipeline: pipeline,
        cursor: {},
        apiStrict: true,
        apiVersion: "1"
    }),
                                 errorCodes,
                                 pipeline);
}

function assertAggregateSucceedsWithAPIStrict(pipeline) {
    assert.commandWorked(db.runCommand(
        {aggregate: collName, pipeline: pipeline, cursor: {}, apiStrict: true, apiVersion: "1"}));
}

function assertViewFailsWithAPIStrict(pipeline) {
    assert.commandFailedWithCode(db.runCommand({
        create: 'new_50_feature_view',
        viewOn: collName,
        pipeline: pipeline,
        apiStrict: true,
        apiVersion: "1"
    }),
                                 ErrorCodes.APIStrictError,
                                 pipeline);
}

function assertViewSucceedsWithAPIStrict(pipeline) {
    assert.commandWorked(db.runCommand({
        create: 'new_50_feature_view',
        viewOn: collName,
        pipeline: pipeline,
        apiStrict: true,
        apiVersion: "1"
    }));

    assert.commandWorked(db.runCommand({drop: 'new_50_feature_view'}));
}

for (let pipeline of stablePipelines) {
    // Assert error thrown when running a pipeline with stages not in API Version 1.
    APIVersionHelpers.assertAggregateSucceedsWithAPIStrict(pipeline, collName);

    // Assert error thrown when creating a view on a pipeline with stages not in API Version 1.
    assertViewSucceedsWithAPIStrict(pipeline);

    // Assert error is not thrown when running without apiStrict=true.
    assert.commandWorked(db.runCommand({
        aggregate: coll.getName(),
        pipeline: pipeline,
        apiVersion: "1",
        cursor: {},
    }));
}

// $setWindowFields is not supported in transactions or with read concern snapshot. Test separately
// and check for all the error codes that can occur depending on what passthrough we are in.
const setWindowFieldsPipeline = [{
    $setWindowFields: {
        sortBy: {_id: 1},
        output: {runningCount: {$sum: 1, window: {documents: ["unbounded", "current"]}}}
    }
}];
assertAggregateSucceedsWithAPIStrict(setWindowFieldsPipeline);

APIVersionHelpers.assertAggregateSucceedsWithAPIStrict(
    setWindowFieldsPipeline,
    collName,
    [ErrorCodes.InvalidOptions, ErrorCodes.OperationNotSupportedInTransaction]);

APIVersionHelpers.assertViewSucceedsWithAPIStrict(setWindowFieldsPipeline, collName);

// Creating a collection with dotted paths is allowed with apiStrict:true.

assert.commandWorked(db.runCommand({
    create: 'new_50_features_validator',
    validator: {$expr: {$eq: [{$getField: {input: "$$ROOT", field: "dotted.path"}}, 2]}},
    apiVersion: "1",
    apiStrict: true
}));

assert.commandWorked(db.runCommand({drop: 'new_50_features_validator'}));
})();
