"use strict";

/**
 * Helper functions that help make assertions on API Version parameters.
 */
var APIVersionHelpers = (function() {
    /**
     * Asserts that the pipeline fails with the given code when apiStrict is set to true and
     * apiVersion is "1".
     */
    function assertAggregateFailsWithAPIStrict(pipeline, collName, errorCodes) {
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

    /**
     * Asserts that the pipeline succeeds when apiStrict is set to true and
     * apiVersion is "1".
     */
    function assertAggregateSucceedsWithAPIStrict(pipeline, collName, errorCodes) {
        if (errorCodes) {
            assert.commandWorkedOrFailedWithCode(db.runCommand({
                aggregate: collName,
                pipeline: pipeline,
                cursor: {},
                apiStrict: true,
                apiVersion: "1"
            }),
                                                 errorCodes,
                                                 pipeline);
        } else {
            assert.commandWorked(db.runCommand({
                aggregate: collName,
                pipeline: pipeline,
                cursor: {},
                apiStrict: true,
                apiVersion: "1"
            }),
                                 pipeline);
        }
    }

    /**
     * Asserts that the given pipeline cannot be used to define a view when apiStrict is set to true
     * and apiVersion is "1" on the create command.
     */
    function assertViewFailsWithAPIStrict(pipeline, viewName, collName) {
        assert.commandFailedWithCode(db.runCommand({
            create: viewName,
            viewOn: collName,
            pipeline: pipeline,
            apiStrict: true,
            apiVersion: "1"
        }),
                                     ErrorCodes.APIStrictError,
                                     pipeline);
    }

    /**
     * Asserts that the given pipeline can be used to define a view when apiStrict is set to true
     * and apiVersion is "1" on the create command.
     */
    function assertViewSucceedsWithAPIStrict(pipeline, viewName, collName) {
        assert.commandWorked(db.runCommand({
            create: viewName,
            viewOn: collName,
            pipeline: pipeline,
            apiStrict: true,
            apiVersion: "1"
        }));

        assert.commandWorked(db.runCommand({drop: viewName}));
    }

    return {
        assertAggregateFailsWithAPIStrict: assertAggregateFailsWithAPIStrict,
        assertAggregateSucceedsWithAPIStrict: assertAggregateSucceedsWithAPIStrict,
        assertViewFailsWithAPIStrict: assertViewFailsWithAPIStrict,
        assertViewSucceedsWithAPIStrict: assertViewSucceedsWithAPIStrict,
    };
})();
