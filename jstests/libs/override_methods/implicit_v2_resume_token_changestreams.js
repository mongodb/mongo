/**
 * Loading this file overrides 'runCommand' with a function that modifies any $changeStream
 * aggregation to use $_generateV2ResumeTokens:true.
 * TODO SERVER-65370: remove this suite when v2 tokens become the default, or rework it to test v1.
 */
(function() {
"use strict";

load("jstests/libs/override_methods/override_helpers.js");  // For 'OverrideHelpers'.

// Override runCommand to set $_generateV2ResumeTokens on all $changeStreams.
function runCommandV2Tokens(conn, dbName, cmdName, cmdObj, originalRunCommand, makeRunCommandArgs) {
    if (OverrideHelpers.isAggregationWithChangeStreamStage(cmdName, cmdObj)) {
        // Make a copy to avoid mutating the user's original command object.
        cmdObj = Object.assign({}, cmdObj, {$_generateV2ResumeTokens: true});
    }
    return originalRunCommand.apply(conn, makeRunCommandArgs(cmdObj));
}

// Always apply the override if a test spawns a parallel shell.
OverrideHelpers.prependOverrideInParallelShell(
    "jstests/libs/override_methods/implicit_v2_resume_token_changestreams.js");

// Override the default runCommand with our custom version.
OverrideHelpers.overrideRunCommand(runCommandV2Tokens);
})();