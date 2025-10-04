/**
 * Loading this file overrides Mongo.prototype.runCommand() with a function that wraps any
 * aggregate command's pipeline inside a $facet stage, then appends an $unwind stage. This will
 * yield the same results, but stress the logic of the $facet stage.
 */

// Set the batch size of the $facet stage's buffer to be lower. This will further stress the
// batching logic, since most pipelines will fall below the default size of 100MB.
assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryFacetBufferSizeBytes: 1000}));

// Save a reference to the original runCommand method in the IIFE's scope.
// This scoping allows the original method to be called by the override below.
let originalRunCommand = Mongo.prototype.runCommand;

Mongo.prototype.runCommand = function (dbName, cmdObj, options) {
    // Skip wrapping the pipeline in a $facet stage if it's not an aggregation, or if it's
    // possibly an invalid one without a pipeline.
    if (
        typeof cmdObj !== "object" ||
        cmdObj === null ||
        !cmdObj.hasOwnProperty("aggregate") ||
        !cmdObj.hasOwnProperty("pipeline") ||
        !Array.isArray(cmdObj.pipeline)
    ) {
        return originalRunCommand.apply(this, arguments);
    }

    let originalPipeline = cmdObj.pipeline;

    const stagesDisallowedInsideFacet = [
        "$changeStream",
        "$collStats",
        "$facet",
        "$geoNear",
        "$indexStats",
        "$merge",
        "$out",
    ];
    for (let stageSpec of originalPipeline) {
        // Skip wrapping the pipeline in a $facet stage if it has an invalid stage
        // specification.
        if (typeof stageSpec !== "object" || stageSpec === null) {
            jsTest.log.info("Not wrapping invalid pipeline in a $facet stage");
            return originalRunCommand.apply(this, arguments);
        }

        if (stageSpec.hasOwnProperty("$match") && typeof stageSpec.$match === "object" && stageSpec.$match !== null) {
            if (stageSpec.$match.hasOwnProperty("$text")) {
                // A $text search is disallowed within a $facet stage.
                jsTest.log.info("Not wrapping $text in a $facet stage");
                return originalRunCommand.apply(this, arguments);
            }
            if (Object.keys(stageSpec.$match).length === 0) {
                // Skip wrapping an empty $match stage, since it can be optimized out, resulting
                // in an empty pipeline which is disallowed within a $facet stage.
                jsTest.log.info("Not wrapping empty $match in a $facet stage");
                return originalRunCommand.apply(this, arguments);
            }
        }

        // Skip wrapping the pipeline in a $facet stage if it contains a stage disallowed inside
        // a $facet.
        for (let disallowedStage of stagesDisallowedInsideFacet) {
            if (stageSpec.hasOwnProperty(disallowedStage)) {
                jsTest.log.info("Not wrapping " + disallowedStage + " in a $facet stage");
                return originalRunCommand.apply(this, arguments);
            }
        }
    }

    cmdObj.pipeline = [
        {$facet: {originalPipeline: originalPipeline, extraPipeline: [{$count: "count"}]}},
        {$unwind: "$originalPipeline"},
        {$replaceRoot: {newRoot: "$originalPipeline"}},
    ];
    return originalRunCommand.apply(this, arguments);
};
