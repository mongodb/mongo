/**
 * Checks that the $_testApiVersion expression used for API versioning testing
 * throws errors as expected.
 *
 * Tests which create views aren't expected to work when collections are implicitly sharded.
 * @tags: [
 *   assumes_unsharded_collection,
 *   # Assumes FCV remain stable during the entire duration of the test
 *   # TODO SERVER-92954: remove once validation of validator during creation of tracked collection
 *   # is fixed.
 *   cannot_run_during_upgrade_downgrade,
 *   uses_api_parameters,
 *   no_selinux,
 *   references_foreign_collection,
 * ]
 */

import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {getExecutionStats} from "jstests/libs/query/analyze_plan.js";

const testDb = db.getSiblingDB(jsTestName());
const collName = "api_version_test_expression";
const coll = testDb[collName];
coll.drop();
const collForeignName = collName + "_foreign";
const collForeign = testDb[collForeignName];
collForeign.drop();

// TODO SERVER-92954 remove this helper function after validation of validator during creation of
// tracked collection is fixed
function useShardingCoordinatorForCreate() {
    return (FixtureHelpers.isMongos(db) || TestData.testingReplicaSetEndpoint) &&
        TestData.implicitlyTrackUnshardedCollectionOnCreation;
}

for (let i = 0; i < 5; i++) {
    assert.commandWorked(coll.insert({num: i}));
    assert.commandWorked(collForeign.insert({num: i}));
}

// Assert error thrown when command specifies {apiStrict: true} and expression specifies {unstable:
// true}.
let pipeline = [{$project: {v: {$_testApiVersion: {unstable: true}}}}];
assert.commandFailedWithCode(
    testDb.runCommand(
        {aggregate: collName, pipeline: pipeline, cursor: {}, apiStrict: true, apiVersion: "1"}),
    ErrorCodes.APIStrictError);

// Assert error thrown when command specifies {apiDeprecationErrors: true} and expression specifies
// {deprecated: true}
pipeline = [{$project: {v: {$_testApiVersion: {deprecated: true}}}}];
assert.commandFailedWithCode(testDb.runCommand({
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
assert.commandFailedWithCode(testDb.runCommand({
    aggregate: collName,
    pipeline: [{$lookup: {from: collForeignName, as: "output", pipeline: unstableInnerPipeline}}],
    cursor: {},
    apiStrict: true,
    apiVersion: "1"
}),
                             ErrorCodes.APIStrictError);
assert.commandFailedWithCode(testDb.runCommand({
    aggregate: collName,
    pipeline: [{$unionWith: {coll: collForeignName, pipeline: unstableInnerPipeline}}],
    cursor: {},
    apiStrict: true,
    apiVersion: "1"
}),
                             ErrorCodes.APIStrictError);

// Assert command worked when the command specifies apiStrict:false and an inner pipeline contains
// an unstable expression.
assert.commandWorked(testDb.runCommand({
    aggregate: collName,
    pipeline: [{$lookup: {from: collForeignName, as: "output", pipeline: unstableInnerPipeline}}],
    cursor: {},
    apiStrict: false,
    apiVersion: "1"
}));
assert.commandWorked(testDb.runCommand({
    aggregate: collName,
    pipeline: [{$unionWith: {coll: collForeignName, pipeline: unstableInnerPipeline}}],
    cursor: {},
    apiStrict: false,
    apiVersion: "1"
}));

// Assert error thrown when the command specifies apiDeprecationErrors:true and an inner pipeline
// contains a deprecated expression.
const deprecatedInnerPipeline = [{$project: {v: {$_testApiVersion: {deprecated: true}}}}];
assert.commandFailedWithCode(testDb.runCommand({
    aggregate: collName,
    pipeline: [{$lookup: {from: collForeignName, as: "output", pipeline: deprecatedInnerPipeline}}],
    cursor: {},
    apiDeprecationErrors: true,
    apiVersion: "1"
}),
                             ErrorCodes.APIDeprecationError);
assert.commandFailedWithCode(testDb.runCommand({
    aggregate: collName,
    pipeline: [{$unionWith: {coll: collForeignName, pipeline: deprecatedInnerPipeline}}],
    cursor: {},
    apiDeprecationErrors: true,
    apiVersion: "1"
}),
                             ErrorCodes.APIDeprecationError);

// Assert command worked when the command specifies apiDeprecationErrors:false and an inner pipeline
// contains a deprecated expression.
assert.commandWorked(testDb.runCommand({
    aggregate: collName,
    pipeline: [{$lookup: {from: collForeignName, as: "output", pipeline: deprecatedInnerPipeline}}],
    cursor: {},
    apiDeprecationErrors: false,
    apiVersion: "1"
}));
assert.commandWorked(testDb.runCommand({
    aggregate: collName,
    pipeline: [{$unionWith: {coll: collForeignName, pipeline: deprecatedInnerPipeline}}],
    cursor: {},
    apiDeprecationErrors: false,
    apiVersion: "1"
}));

// Test that command successfully runs to completion without any API parameters.
pipeline = [{$project: {v: {$_testApiVersion: {unstable: true}}}}];
assert.commandWorked(testDb.runCommand({aggregate: collName, pipeline: pipeline, cursor: {}}));

// Create a view with {apiStrict: true}.
testDb.view.drop();
assert.commandWorked(testDb.runCommand(
    {create: "view", viewOn: collName, pipeline: [], apiStrict: true, apiVersion: "1"}));
// find() on views should work normally if 'apiStrict' is true.
assert.commandWorked(testDb.runCommand({find: "view", apiStrict: true, apiVersion: "1"}));
// This command will work because API parameters are not inherited from views.
assert.commandWorked(testDb.runCommand({aggregate: "view", pipeline: pipeline, cursor: {}}));
assert.commandFailedWithCode(
    testDb.runCommand(
        {aggregate: "view", pipeline: pipeline, cursor: {}, apiVersion: "1", apiStrict: true}),
    ErrorCodes.APIStrictError);

// Create a view with 'unstable' parameter should fail with 'apiStrict'.
testDb.unstableView.drop();
assert.commandFailedWithCode(testDb.runCommand({
    create: "unstableView",
    viewOn: collName,
    pipeline: pipeline,
    apiStrict: true,
    apiVersion: "1"
}),
                             ErrorCodes.APIStrictError);

// Create a view with 'unstable' should be allowed without 'apiStrict'.
assert.commandWorked(testDb.runCommand({
    create: "unstableView",
    viewOn: collName,
    pipeline: pipeline,
    apiVersion: "1",
    apiStrict: false
}));
assert.commandWorked(testDb.runCommand({aggregate: "unstableView", pipeline: [], cursor: {}}));

// This commmand will fail even with the empty pipeline because of the view.
assert.commandFailedWithCode(
    testDb.runCommand(
        {aggregate: "unstableView", pipeline: [], cursor: {}, apiVersion: "1", apiStrict: true}),
    ErrorCodes.APIStrictError);

// Create a validator containing the unstable test expression.
let validator = {$expr: {$_testApiVersion: {unstable: true}}};
let validatedCollName = collName + "_validated";

if (!useShardingCoordinatorForCreate()) {
    // Creating a collection with the unstable validator is not allowed with apiStrict:true.
    testDb[validatedCollName].drop();
    assert.commandFailedWithCode(
        testDb.runCommand(
            {create: validatedCollName, validator: validator, apiVersion: "1", apiStrict: true}),
        ErrorCodes.APIStrictError);
}

// Run create and insert commands without apiStrict:true and verify that it is successful.
assert.commandWorked(testDb.runCommand(
    {create: validatedCollName, validator: validator, apiVersion: "1", apiStrict: false}));
assert.commandWorked(
    testDb[validatedCollName].runCommand({insert: validatedCollName, documents: [{num: 1}]}));

// Specifying apiStrict: true results in an error.
assert.commandFailedWithCode(
    testDb[validatedCollName].runCommand(
        {insert: validatedCollName, documents: [{num: 1}], apiVersion: "1", apiStrict: true}),
    ErrorCodes.APIStrictError);

// Recreate the validator containing a deprecated test expression.
testDb[validatedCollName].drop();
validator = {
    $expr: {$_testApiVersion: {deprecated: true}}
};

if (!useShardingCoordinatorForCreate()) {
    // Creating a collection with the deprecated validator is not allowed with
    // apiDeprecationErrors:true.
    assert.commandFailedWithCode(testDb.runCommand({
        create: validatedCollName,
        validator: validator,
        apiVersion: "1",
        apiDeprecationErrors: true,
    }),
                                 ErrorCodes.APIDeprecationError);
}

// Run create and insert commands without apiDeprecationErrors:true and verify that it is
// successful.
assert.commandWorked(testDb.runCommand({
    create: validatedCollName,
    validator: validator,
    apiVersion: "1",
    apiDeprecationErrors: false,
}));
assert.commandWorked(
    testDb[validatedCollName].runCommand({insert: validatedCollName, documents: [{num: 1}]}));

// Specifying apiDeprecationErrors: true results in an error.
assert.commandFailedWithCode(testDb[validatedCollName].runCommand({
    insert: validatedCollName,
    documents: [{num: 1}],
    apiVersion: "1",
    apiDeprecationErrors: true
}),
                             ErrorCodes.APIDeprecationError);

// Test that API version parameters are inherited into the inner command of the explain command.
function checkExplainInnerCommandGetsAPIVersionParameters(explainedCmd, errCode) {
    assert.commandFailedWithCode(
        testDb.runCommand(
            {explain: explainedCmd, apiVersion: "1", apiDeprecationErrors: true, apiStrict: true}),
        errCode);

    // If 'apiStrict: false' the inner aggregate command will execute successfully.
    const explainRes =
        testDb.runCommand({explain: explainedCmd, apiVersion: "1", apiStrict: false});
    const executionStats = getExecutionStats(explainRes)[0];
    assert.eq(executionStats.executionSuccess, true, {explainRes, executionStats});
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

testDb[validatedCollName].drop();
testDb.unstableView.drop();
