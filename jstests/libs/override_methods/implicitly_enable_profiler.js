/**
 * Loading this file overrides Mongo.prototype.runCommand to implicitly enable profiling for every
 * accessed database.
 */

import {DiscoverTopology, Topology} from "jstests/libs/discover_topology.js";
import {OverrideHelpers} from "jstests/libs/override_methods/override_helpers.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {TransactionsUtil} from "jstests/libs/transactions_util.js";

// If the collection is being dropped, do not enable profiling since doing so would cause the
// database to be re-created.
const excludedCommandNames = ["dropDatabase"];

const enabledDbNames = new Set([]);

const hosts = [];
const topology = DiscoverTopology.findConnectedNodes(db.getMongo());
if (topology.type === Topology.kReplicaSet) {
    hosts.push(...topology.nodes);
    new ReplSetTest(topology.nodes[0]).awaitSecondaryNodes();
} else {
    throw new Error("Can only enabling profiler on a replica set. Unrecognized topology format: " +
                    tojson(topology));
}
print("Implicitly enabling profiler on " + tojsononeline(hosts));

function runCommandAfterEnablingProfiler(
    conn, dbName, commandName, commandObj, func, makeFuncArgs) {
    if (!excludedCommandNames.includes(commandName) && !enabledDbNames.has(dbName)) {
        // Implicitly enable profiling for this database on all the nodes in the replica set.
        hosts.forEach(host => {
            const conn = new Mongo(host);
            assert.commandWorkedOrFailedWithCode(
                func.apply(conn, makeFuncArgs({profile: 2})),
                [ErrorCodes.InvalidNamespace, ErrorCodes.DatabaseDifferCase]);
        });
        enabledDbNames.add(dbName);
    }
    if (commandName == "dropDatabase") {
        // Remove the name for this database from the set so profiling is re-enabled when the
        // database gets recreated.
        enabledDbNames.delete(dbName);
    }
    if (commandName == "listCollections") {
        // Hide the system.profile collection from the listCollections result since its presence
        // can cause the assertions in some tests to fail.
        commandObj = TransactionsUtil.deepCopyObject({}, commandObj);
        if (!commandObj.hasOwnProperty("filter")) {
            commandObj.filter = {};
        }
        if (Object.keys(commandObj.filter).length == 0) {
            commandObj.filter.name = {$ne: "system.profile"};
        } else {
            commandObj.filter = {$and: [{name: {$ne: "system.profile"}}, commandObj.filter]};
        }
    }
    return func.apply(conn, makeFuncArgs(commandObj));
}

OverrideHelpers.prependOverrideInParallelShell(
    "jstests/libs/override_methods/implicitly_enable_profiler.js");

OverrideHelpers.overrideRunCommand(runCommandAfterEnablingProfiler);
