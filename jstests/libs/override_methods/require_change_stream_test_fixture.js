/**
 * This file defines a command override that disallows all aggregate pipelines with a $changeStream
 * stage if they were not started from within the ChangeStreamTest fixture.
 */
import {OverrideHelpers} from "jstests/libs/override_methods/override_helpers.js";

function runCommandOverride(conn, dbName, cmdName, cmdObj, clientFunction, makeFuncArgs) {
    // Check if the command is an aggregate command with a $changeStream pipeline.
    if (cmdName === "aggregate" && typeof cmdObj === 'object' &&
        cmdObj.hasOwnProperty('pipeline')) {
        const pipeline = cmdObj.pipeline;
        if (Array.isArray(pipeline) && pipeline.length &&
            pipeline[0].hasOwnProperty('$changeStream')) {
            // Now check if the command was started from within the ChangeStreamTest fixture.
            if (!globalThis.isInsideChangeStreamTestFixture) {
                // Not started from within the ChangeStreamTest fixture. This is disallowed.
                throw new Error(
                    "Using a $changeStream pipeline is disallowed from here. Please use the ChangeStreamTest fixture instead.");
            }
            // Command started from within the ChangeStreamTest fixture. We will let it pass
            // through.
        }
    }
    // Call the original function.
    return clientFunction.apply(conn, makeFuncArgs(cmdObj));
}

// Override the default runCommand with our custom version.
OverrideHelpers.overrideRunCommand(runCommandOverride);
