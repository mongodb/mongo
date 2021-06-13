/**
 * Checks that the $_testApiVersion expression used for API versioning testing
 * throws errors as expected.
 *
 * Tests which create views aren't expected to work when collections are implicitly sharded.
 * @tags: [
 *   assumes_against_mongod_not_mongos,
 *   assumes_unsharded_collection,
 *   requires_fcv_50,
 *   uses_api_parameters,
 * ]
 */

(function() {
"use strict";

const collName = "api_version_test_expression";
const coll = db[collName];
coll.drop();
const collForeignName = collName + "_foreign";
const collForeign = db[collForeignName];
collForeign.drop();

for (let i = 0; i < 5; i++) {
    assert.commandWorked(coll.insert({num: i}));
    assert.commandWorked(collForeign.insert({num: i}));
}

// Assert error thrown when command specifies {apiStrict: true} and expression specifies {unstable:
// true}.
let pipeline = [{$project: {v: {$_testApiVersion: {unstable: true}}}}];
assert.commandFailedWithCode(
    db.runCommand(
        {aggregate: collName, pipeline: pipeline, cursor: {}, apiStrict: true, apiVersion: "1"}),
    ErrorCodes.APIStrictError);

// Assert error thrown when command specifies {apiDeprecationErrors: true} and expression specifies
// {deprecated: true}
pipeline = [{$project: {v: {$_testApiVersion: {deprecated: true}}}}];
assert.commandFailedWithCode(db.runCommand({
    aggregate: collName,
    pipeline: pipeline,
    cursor: {},
    apiDeprecationErrors: true,
    apiVersion: "1"
}),
                             ErrorCodes.APIDeprecationError);

// Assert error thrown when the command specifies apiStrict:true and an inner pipeline contains an
// unstable expression.
const unstableInnerPipeline = [{$project: {v: {$_testApiVersion: {unstable: true}}}}];
assert.commandFailedWithCode(db.runCommand({
    aggregate: collName,
    pipeline: [{$lookup: {from: collForeignName, as: "output", pipeline: unstableInnerPipeline}}],
    cursor: {},
    apiStrict: true,
    apiVersion: "1"
}),
                             ErrorCodes.APIStrictError);
assert.commandFailedWithCode(db.runCommand({
    aggregate: collName,
    pipeline: [{$unionWith: {coll: collForeignName, pipeline: unstableInnerPipeline}}],
    cursor: {},
    apiStrict: true,
    apiVersion: "1"
}),
                             ErrorCodes.APIStrictError);

// Assert command worked when the command specifies apiStrict:false and an inner pipeline contains
// an unstable expression.
assert.commandWorked(db.runCommand({
    aggregate: collName,
    pipeline: [{$lookup: {from: collForeignName, as: "output", pipeline: unstableInnerPipeline}}],
    cursor: {},
    apiStrict: false,
    apiVersion: "1"
}));
assert.commandWorked(db.runCommand({
    aggregate: collName,
    pipeline: [{$unionWith: {coll: collForeignName, pipeline: unstableInnerPipeline}}],
    cursor: {},
    apiStrict: false,
    apiVersion: "1"
}));

// Assert error thrown when the command specifies apiDeprecationErrors:true and an inner pipeline
// contains a deprecated expression.
const deprecatedInnerPipeline = [{$project: {v: {$_testApiVersion: {deprecated: true}}}}];
assert.commandFailedWithCode(db.runCommand({
    aggregate: collName,
    pipeline: [{$lookup: {from: collForeignName, as: "output", pipeline: deprecatedInnerPipeline}}],
    cursor: {},
    apiDeprecationErrors: true,
    apiVersion: "1"
}),
                             ErrorCodes.APIDeprecationError);
assert.commandFailedWithCode(db.runCommand({
    aggregate: collName,
    pipeline: [{$unionWith: {coll: collForeignName, pipeline: deprecatedInnerPipeline}}],
    cursor: {},
    apiDeprecationErrors: true,
    apiVersion: "1"
}),
                             ErrorCodes.APIDeprecationError);

// Assert command worked when the command specifies apiDeprecationErrors:false and an inner pipeline
// contains a deprecated expression.
assert.commandWorked(db.runCommand({
    aggregate: collName,
    pipeline: [{$lookup: {from: collForeignName, as: "output", pipeline: deprecatedInnerPipeline}}],
    cursor: {},
    apiDeprecationErrors: false,
    apiVersion: "1"
}));
assert.commandWorked(db.runCommand({
    aggregate: collName,
    pipeline: [{$unionWith: {coll: collForeignName, pipeline: deprecatedInnerPipeline}}],
    cursor: {},
    apiDeprecationErrors: false,
    apiVersion: "1"
}));

// Test that command successfully runs to completion without any API parameters.
pipeline = [{$project: {v: {$_testApiVersion: {unstable: true}}}}];
assert.commandWorked(db.runCommand({aggregate: collName, pipeline: pipeline, cursor: {}}));

// Create a view with {apiStrict: true}.
db.view.drop();
assert.commandWorked(db.runCommand(
    {create: "view", viewOn: collName, pipeline: [], apiStrict: true, apiVersion: "1"}));
// find() on views should work normally if 'apiStrict' is true.
assert.commandWorked(db.runCommand({find: "view", apiStrict: true, apiVersion: "1"}));
// This command will work because API parameters are not inherited from views.
assert.commandWorked(db.runCommand({aggregate: "view", pipeline: pipeline, cursor: {}}));
assert.commandFailedWithCode(
    db.runCommand(
        {aggregate: "view", pipeline: pipeline, cursor: {}, apiVersion: "1", apiStrict: true}),
    ErrorCodes.APIStrictError);

// Create a view with 'unstable' parameter should fail with 'apiStrict'.
db.unstableView.drop();
assert.commandFailedWithCode(db.runCommand({
    create: "unstableView",
    viewOn: collName,
    pipeline: pipeline,
    apiStrict: true,
    apiVersion: "1"
}),
                             ErrorCodes.APIStrictError);

// Create a view with 'unstable' should be allowed without 'apiStrict'.
assert.commandWorked(db.runCommand({
    create: "unstableView",
    viewOn: collName,
    pipeline: pipeline,
    apiVersion: "1",
    apiStrict: false
}));
assert.commandWorked(db.runCommand({aggregate: "unstableView", pipeline: [], cursor: {}}));

// This commmand will fail even with the empty pipeline because of the view.
assert.commandFailedWithCode(
    db.runCommand(
        {aggregate: "unstableView", pipeline: [], cursor: {}, apiVersion: "1", apiStrict: true}),
    ErrorCodes.APIStrictError);

// Create a validator containing the unstable test expression.
let validator = {$expr: {$_testApiVersion: {unstable: true}}};
let validatedCollName = collName + "_validated";

// Creating a collection with the unstable validator is not allowed with apiStrict:true.
db[validatedCollName].drop();
assert.commandFailedWithCode(
    db.runCommand(
        {create: validatedCollName, validator: validator, apiVersion: "1", apiStrict: true}),
    ErrorCodes.APIStrictError);

// Run create and insert commands without apiStrict:true and verify that it is successful.
assert.commandWorked(db.runCommand(
    {create: validatedCollName, validator: validator, apiVersion: "1", apiStrict: false}));
assert.commandWorked(
    db[validatedCollName].runCommand({insert: validatedCollName, documents: [{num: 1}]}));

// Specifying apiStrict: true results in an error.
assert.commandFailedWithCode(
    db[validatedCollName].runCommand(
        {insert: validatedCollName, documents: [{num: 1}], apiVersion: "1", apiStrict: true}),
    ErrorCodes.APIStrictError);

// Recreate the validator containing a deprecated test expression.
db[validatedCollName].drop();
validator = {
    $expr: {$_testApiVersion: {deprecated: true}}
};

// Creating a collection with the deprecated validator is not allowed with
// apiDeprecationErrors:true.
assert.commandFailedWithCode(db.runCommand({
    create: validatedCollName,
    validator: validator,
    apiVersion: "1",
    apiDeprecationErrors: true,
}),
                             ErrorCodes.APIDeprecationError);

// Run create and insert commands without apiDeprecationErrors:true and verify that it is
// successful.
assert.commandWorked(db.runCommand({
    create: validatedCollName,
    validator: validator,
    apiVersion: "1",
    apiDeprecationErrors: false,
}));
assert.commandWorked(
    db[validatedCollName].runCommand({insert: validatedCollName, documents: [{num: 1}]}));

// Specifying apiDeprecationErrors: true results in an error.
assert.commandFailedWithCode(db[validatedCollName].runCommand({
    insert: validatedCollName,
    documents: [{num: 1}],
    apiVersion: "1",
    apiDeprecationErrors: true
}),
                             ErrorCodes.APIDeprecationError);

// Test that API version parameters are inherited into the inner command of the explain command.
function checkExplainInnerCommandGetsAPIVersionParameters(explainedCmd, errCode) {
    assert.commandFailedWithCode(
        db.runCommand(
            {explain: explainedCmd, apiVersion: "1", apiDeprecationErrors: true, apiStrict: true}),
        errCode);

    // If 'apiStrict: false' the inner aggregate command will execute successfully.
    const explainRes = db.runCommand({explain: explainedCmd, apiVersion: "1", apiStrict: false});
    assert(explainRes.hasOwnProperty('executionStats'), explainRes);
    assert.eq(explainRes['executionStats']['executionSuccess'], true, explainRes);
}

pipeline = [{$project: {v: {$_testApiVersion: {unstable: true}}}}];
let aggCmd = {aggregate: collName, pipeline: pipeline, cursor: {}};
checkExplainInnerCommandGetsAPIVersionParameters(aggCmd, ErrorCodes.APIStrictError);

let findCmd = {find: collName, projection: {v: {$_testApiVersion: {unstable: true}}}};
checkExplainInnerCommandGetsAPIVersionParameters(findCmd, ErrorCodes.APIStrictError);

pipeline = [{$project: {v: {$_testApiVersion: {deprecated: true}}}}];
aggCmd = {
    aggregate: collName,
    pipeline: pipeline,
    cursor: {}
};
checkExplainInnerCommandGetsAPIVersionParameters(aggCmd, ErrorCodes.APIDeprecationError);

findCmd = {
    find: collName,
    projection: {v: {$_testApiVersion: {deprecated: true}}}
};
checkExplainInnerCommandGetsAPIVersionParameters(findCmd, ErrorCodes.APIDeprecationError);

db[validatedCollName].drop();
db.unstableView.drop();
})();
