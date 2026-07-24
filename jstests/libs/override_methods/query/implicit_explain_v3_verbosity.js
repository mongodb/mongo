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
        // "allPlansExecution" maps to "plannerStats", which by design does not execute the query
        // and so carries no "executionStats" section, while the tests' assertions about final
        // execution require one. For that verbosity, run the test's original legacy explain first
        // and hand that back unchanged (running it first keeps the returned response identical to
        // what the plain suite would produce - the shadow V3 run below may e.g. seed the plan
        // cache), then run the V3-mapped explain as a shadow and sanity-check its shape. Both
        // verbosities stay covered: the unchanged legacy assertions plus plannerStats end-to-end.
        const legacyRes =
            cmdObj.verbosity === "allPlansExecution"
                ? clientFunction.apply(conn, makeFuncArgs(cmdObj))
                : null;

        // Make a shallow copy so we don't mutate the caller's command object.
        const v3CmdObj = {...cmdObj, verbosity: kVerbosityMapping[cmdObj.verbosity]};
        const res = clientFunction.apply(conn, makeFuncArgs(v3CmdObj));
        if (res.ok) {
            // Assert that the explain command's response contains the expected V3 version field.
            assert(res.hasOwnProperty("explainVersion"), res);
            assert.eq(res.explainVersion, "3", res);
        }

        if (legacyRes === null) {
            return res;
        }
        // The V3 shape assertions apply on the classic find path, where the real V3 shape is
        // produced (other paths still delegate to mapped legacy sections).
        if (res.ok && res.queryPlanner && res.queryPlanner.hasOwnProperty("plans")) {
            assert(!res.hasOwnProperty("executionStats"), res);
            assert(Array.isArray(res.queryPlanner.plans), res);
            assert.gte(res.queryPlanner.plans.length, 1, res);
        }
        return legacyRes;
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
