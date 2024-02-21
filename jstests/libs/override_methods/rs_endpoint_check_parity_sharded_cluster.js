/**
 * Loading this file overrides Mongo.prototype.runCommand to compare the response from a
 * single-shard cluster connected using replica set endpoint with the response from a single-shard
 * cluster connected using a router.
 */

import {OverrideHelpers} from "jstests/libs/override_methods/override_helpers.js";
import {
    runCommandBase,
} from "jstests/libs/override_methods/rs_endpoint_check_parity_util.js";

assert(TestData.testingReplicaSetEndpoint, "Expect testing replica set endpoint");

function runCommandCompareResponses(conn, dbName, commandName, commandObj, func, makeFuncArgs) {
    // TODO (SERVER-86286): Run the command against the sharded clusters described above.
    return runCommandBase(conn, dbName, commandName, commandObj, func, makeFuncArgs);
}

OverrideHelpers.prependOverrideInParallelShell(
    "jstests/libs/override_methods/check_parity_sharded_cluster.js");

OverrideHelpers.overrideRunCommand(runCommandCompareResponses);
