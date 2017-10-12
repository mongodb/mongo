// Test that query and aggregation syntax introduced in 3.6 is restricted from view definitions when
// the featureCompatibilityVersion is 3.4.
// TODO SERVER-31588: Remove FCV 3.4 validation during the 3.7 development cycle.

(function() {
    "use strict";
    const conn = MongoRunner.runMongod({});
    assert.neq(null, conn, "mongod was unable to start up");

    const testDB = conn.getDB("view_definition_feature_compatibility_version");
    assert.commandWorked(testDB.dropDatabase());

    const adminDB = conn.getDB("admin");

    function pipelineValidForViewUnderFCV_36(pipeline) {
        // Set featureCompatibilityVersion to 3.6.
        assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: "3.6"}));

        // Confirm view creation with 'pipeline' succeeds.
        assert.commandWorked(testDB.createView("view", "test", pipeline));
        assert(testDB.view.drop());

        // Confirm view collMod with 'pipeline' succeeds.
        assert.commandWorked(testDB.createView("view", "test", []));
        assert.commandWorked(
            testDB.runCommand({collMod: "view", viewOn: "test", pipeline: pipeline}));
        assert(testDB.view.drop());
    }

    function pipelineNotValidForViewUnderFCV_34(pipeline) {
        // Set featureCompatibilityVersion to 3.4.
        assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: "3.4"}));

        // Confirm view creation with 'pipeline' fails.
        assert.commandFailedWithCode(testDB.createView("view", "test", pipeline),
                                     ErrorCodes.QueryFeatureNotAllowed);

        // Confirm view collMod with 'pipeline' fails.
        assert.commandWorked(testDB.createView("view", "test", []));
        assert.commandFailedWithCode(
            testDB.runCommand({collMod: "view", viewOn: "test", pipeline: pipeline}),
            ErrorCodes.QueryFeatureNotAllowed);
        assert(testDB.view.drop());
    }

    // $lookup with 'pipeline' syntax.
    let pipeline = [{$lookup: {from: "test", as: "as", pipeline: []}}];
    pipelineValidForViewUnderFCV_36(pipeline);
    pipelineNotValidForViewUnderFCV_34(pipeline);

    // $match with $jsonSchema.
    pipeline = [{$match: {$jsonSchema: {"required": ["x"]}}}];
    pipelineValidForViewUnderFCV_36(pipeline);
    pipelineNotValidForViewUnderFCV_34(pipeline);

    // $match with $expr.
    pipeline = [{$match: {$expr: {$eq: ["$x", "$y"]}}}];
    pipelineValidForViewUnderFCV_36(pipeline);
    pipelineNotValidForViewUnderFCV_34(pipeline);

    // $facet with a subpipeline containing a $match with $jsonSchema.
    pipeline = [{$facet: {output: [{$match: {$jsonSchema: {"required": ["x"]}}}]}}];
    pipelineValidForViewUnderFCV_36(pipeline);
    pipelineNotValidForViewUnderFCV_34(pipeline);

    // $facet with a subpipeline containing a $match with $expr.
    pipeline = [{$facet: {output: [{$match: {$expr: {$eq: ["$x", "$y"]}}}]}}];
    pipelineValidForViewUnderFCV_36(pipeline);
    pipelineNotValidForViewUnderFCV_34(pipeline);

    MongoRunner.stopMongod(conn);
}());
