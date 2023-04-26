/**
 * Loading this file overrides 'runCommand' with a function that modifies any $changeStream
 * aggregation to use $_generateV2ResumeTokens:false.
 */
(function() {
"use strict";

load("jstests/libs/override_methods/override_helpers.js");  // For 'OverrideHelpers'.

// Override runCommand to set $_generateV2ResumeTokens on all $changeStreams.
function runCommandV1Tokens(conn, dbName, cmdName, cmdObj, originalRunCommand, makeRunCommandArgs) {
    if (OverrideHelpers.isAggregationWithChangeStreamStage(cmdName, cmdObj)) {
        // Make a copy to avoid mutating the user's original command object.
        cmdObj = Object.assign({}, cmdObj, {$_generateV2ResumeTokens: false});
    }
    return originalRunCommand.apply(conn, makeRunCommandArgs(cmdObj));
}

// Always apply the override if a test spawns a parallel shell.
OverrideHelpers.prependOverrideInParallelShell(
    "jstests/libs/override_methods/implicit_v1_resume_token_changestreams.js");

// Override the default runCommand with our custom version.
OverrideHelpers.overrideRunCommand(runCommandV1Tokens);
})();
