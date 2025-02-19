/**
 * Overrides Mongo.prototype.runCommand for query settings supported commands in order to run
 * explain on them multiple times in a row and ensure that the reported 'queryShapeHash' value is
 * same.
 **/
import {getCommandName, getExplainCommand, getInnerCommand} from "jstests/libs/cmd_object_utils.js";
import {DiscoverTopology} from "jstests/libs/discover_topology.js";
import {OverrideHelpers} from "jstests/libs/override_methods/override_helpers.js";
import {QuerySettingsUtils} from "jstests/libs/query/query_settings_utils.js";

/**
 * Assert that all elements of 'array' are identical.
 */
function assertAllEqual(array) {
    assert(array.every(x => x === array[0]), `not all elements are same: ${array}`);
}

/**
 * Given a connection, discover all the cluster connected nodes (both mongod and mongos), and
 * assert that all the explain results for 'explainCmd' have identical query shape hashes.
 */
export function assertQueryShapeHashStability(conn, dbName, explainCmd) {
    try {
        const queryShapeHashes = DiscoverTopology.findNonConfigNodes(conn)
                                     .map(host => new Mongo(host).getDB(dbName))
                                     .map(db => assert.commandWorked(db.runCommand(explainCmd)))
                                     .map(explain => explain.queryShapeHash);
        assertAllEqual(queryShapeHashes);
    } catch (ex) {
        const expectedErrorCodes =
            [ErrorCodes.CommandOnShardedViewNotSupportedOnMongod, ErrorCodes.NamespaceNotFound];
        if (!expectedErrorCodes.includes(ex.code)) {
            throw ex;
        }
    }
}

function runCommandOverride(conn, dbName, cmdName, cmdObj, clientFunction, makeFuncArgs) {
    // Do not run explain on queries that have 'batchSize' set to zero, as in majority of the tests
    // we are expecting an error when calling getMore() on that cursor.
    const hasBatchSizeZero = cmdObj.cursor && cmdObj.cursor.batchSize === 0;
    const res = clientFunction.apply(conn, makeFuncArgs(cmdObj));
    if (res.ok && !hasBatchSizeZero) {
        // Only run the test if the original command works. Some tests assert on commands failing,
        // so we should simply bubble these commands through without any additional checks.
        OverrideHelpers.withPreOverrideRunCommand(() => {
            const innerCmd = getInnerCommand(cmdObj);
            if (!QuerySettingsUtils.isSupportedCommand(getCommandName(innerCmd))) {
                return;
            }
            // Wrap command into explain, if it's not explain yet.
            const explainCmd = getExplainCommand(innerCmd);
            assertQueryShapeHashStability(conn, dbName, explainCmd);
        });
    }
    return res;
}

// Override the default runCommand with our custom version.
OverrideHelpers.overrideRunCommand(runCommandOverride);

// Always apply the override if a test spawns a parallel shell.
OverrideHelpers.prependOverrideInParallelShell(
    "jstests/libs/override_methods/query_shape_hash_stability.js");
