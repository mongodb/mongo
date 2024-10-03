/**
 * Overrides Mongo.prototype.runCommand for query settings supported commands in order to run
 * explain on them multiple times in a row and ensure that the reported 'queryShapeHash' value is
 * same.
 **/
import {getCommandName, getExplainCommand, getInnerCommand} from "jstests/libs/cmd_object_utils.js";
import {OverrideHelpers} from "jstests/libs/override_methods/override_helpers.js";
import {QuerySettingsUtils} from "jstests/libs/query/query_settings_utils.js";

function assertAllEqual(array) {
    assert(array.every(x => x === array[0]), `not all elements are same: ${array}`);
}

function runCommandOverride(conn, dbName, cmdName, cmdObj, clientFunction, makeFuncArgs) {
    function assertQueryShapeHashStability() {
        const innerCmd = getInnerCommand(cmdObj);
        if (!QuerySettingsUtils.isSupportedCommand(getCommandName(innerCmd))) {
            return;
        }

        // Wrap command into explain, if it's not explain yet.
        const db = conn.getDB(dbName);
        const explainCmd = getExplainCommand(innerCmd);

        // Run the explain three times and ensure that all 'queryShapeHash' values are equal.
        assertAllEqual([explainCmd, explainCmd, explainCmd]
                           .map(cmd => assert.commandWorked(db.runCommand(cmd)))
                           .map(explain => explain.queryShapeHash));
    }

    // Do not run explain on queries that have 'batchSize' set to zero, as in majority of the tests
    // we are expecting an error when calling getMore() on that cursor.
    const hasBatchSizeZero = cmdObj.cursor && cmdObj.cursor.batchSize === 0;
    const res = clientFunction.apply(conn, makeFuncArgs(cmdObj));
    if (res.ok && !hasBatchSizeZero) {
        // Only run the test if the original command works. Some tests assert on commands failing,
        // so we should simply bubble these commands through without any additional checks.
        OverrideHelpers.withPreOverrideRunCommand(assertQueryShapeHashStability);
    }
    return res;
}

// Override the default runCommand with our custom version.
OverrideHelpers.overrideRunCommand(runCommandOverride);

// Always apply the override if a test spawns a parallel shell.
OverrideHelpers.prependOverrideInParallelShell(
    "jstests/libs/override_methods/query_shape_hash_stability.js");
