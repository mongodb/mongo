/**
 * Overrides the 'explain' command to implicitly translate the legacy explain verbosities into their
 * V3 counterparts:
 *
 *   queryPlanner       -> plannerChoice
 *   executionStats     -> execStats
 *   allPlansExecution  -> plannerStats
 *
 * This provides coverage of the V3 explain verbosities by running the existing core tests, which
 * request the legacy verbosities, against the new verbosity levels.
 */
import {OverrideHelpers} from "jstests/libs/override_methods/override_helpers.js";

const kVerbosityMapping = {
    queryPlanner: "plannerChoice",
    executionStats: "execStats",
    allPlansExecution: "plannerStats",
};

function runCommandOverride(conn, dbName, cmdName, cmdObj, clientFunction, makeFuncArgs) {
    // Only intercept the explain command with a translatable 'verbosity' field. Note that the
    // aggregate command's own "explain: true" flag is left untouched.
    if (
        cmdName === "explain" &&
        typeof cmdObj === "object" &&
        cmdObj !== null &&
        typeof cmdObj.verbosity === "string" &&
        kVerbosityMapping.hasOwnProperty(cmdObj.verbosity)
    ) {
        // Make a shallow copy so we don't mutate the caller's command object.
        cmdObj = {...cmdObj, verbosity: kVerbosityMapping[cmdObj.verbosity]};
        const res = clientFunction.apply(conn, makeFuncArgs(cmdObj));
        if (res.ok) {
            // Assert that the explain command's response contains the expected V3 version field.
            assert(res.hasOwnProperty("explainVersion"), res);
            assert.eq(res.explainVersion, "3", res);
        }
        return res;
    }

    // Fall back to the default behavior for all other commands.
    return clientFunction.apply(conn, makeFuncArgs(cmdObj));
}

// Override the default runCommand with our custom version.
OverrideHelpers.overrideRunCommand(runCommandOverride);

// Always apply the override if a test spawns a parallel shell.
OverrideHelpers.prependOverrideInParallelShell(
    "jstests/libs/override_methods/query/implicit_explain_v3_verbosity.js",
);
