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
     * Asserts that the given pipeline cannot be used to define a view when apiStrict is set to true
     * and apiVersion is "1" on the create command.
     */
    function assertViewFailsWithAPIStrict(pipeline, collName) {
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

    return {
        assertAggregateFailsWithAPIStrict: assertAggregateFailsWithAPIStrict,
        assertViewFailsWithAPIStrict: assertViewFailsWithAPIStrict,
    };
})();
