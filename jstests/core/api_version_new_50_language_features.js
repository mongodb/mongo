/**
 * Tests that language features introduced in version 4.9 or 5.0 are not included in API Version 1
 * yet. This test should be updated or removed in a future release when we have more confidence that
 * the behavior and syntax is stable.
 *
 * @tags: [
 *   requires_fcv_50,
 *   uses_api_parameters,
 * ]
 */

(function() {
"use strict";

const collName = "api_version_new_50_language_features";
const coll = db[collName];
coll.drop();
assert.commandWorked(coll.insert({a: 1, date: new ISODate()}));

const unstablePipelines = [
    [{$set: {x: {$dateTrunc: {date: "$date", unit: "second", binSize: 5}}}}],
    [{$set: {x: {$dateAdd: {startDate: "$date", unit: "day", amount: 1}}}}],
    [{$set: {x: {$dateSubtract: {startDate: "$date", unit: "day", amount: 1}}}}],
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

for (let pipeline of unstablePipelines) {
    // Assert error thrown when running a pipeline with stages not in API Version 1.
    assertAggregateFailsWithAPIStrict(pipeline, ErrorCodes.APIStrictError);

    // Assert error thrown when creating a view on a pipeline with stages not in API Version 1.
    assertViewFailsWithAPIStrict(pipeline);

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
assertAggregateFailsWithAPIStrict(setWindowFieldsPipeline, [
    ErrorCodes.APIStrictError,
    ErrorCodes.InvalidOptions,
    ErrorCodes.OperationNotSupportedInTransaction
]);

assertViewFailsWithAPIStrict(setWindowFieldsPipeline);

// Creating a collection with the unstable validator is not allowed with apiStrict:true.
assert.commandFailedWithCode(db.runCommand({
    create: 'new_50_features_validator',
    validator: {$expr: {$eq: [{$getField: {input: "$$ROOT", field: "dotted.path"}}, 2]}},
    apiVersion: "1",
    apiStrict: true
}),
                             ErrorCodes.APIStrictError);
})();
