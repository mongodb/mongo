/**
 * This hook runs the reconfig command against the primary of a replica set:
 * The reconfig command first chooses a random node (not the primary) and will change
 * its votes and priority to 0 or 1 depending on the current value.
 *
 * This hook will run concurrently with tests.
 */

'use strict';

(function() {
load('jstests/libs/discover_topology.js');  // For Topology and DiscoverTopology.
load('jstests/libs/parallelTester.js');     // For Thread.

if (typeof db === 'undefined') {
    throw new Error(
        "Expected mongo shell to be connected a mongod, but global 'db' object isn't defined");
}

const conn = db.getMongo();
const topology = DiscoverTopology.findConnectedNodes(conn);

/**
 * Returns true if the error code is transient.
 */
function isIgnorableError(codeName) {
    if (codeName == "ConfigurationInProgress" || codeName == "NotMaster" ||
        codeName == "InterruptedDueToReplStateChange" || codeName == "PrimarySteppedDown") {
        return true;
    }
    return false;
}

/**
 * Runs the reconfig command against the primary of a replica set.
 *
 * The reconfig command randomly chooses a node to change it's votes and priority to 0 or 1
 * based on what the node's current votes and priority fields are. We always check to see that
 * there exists at least two voting nodes in the set, which ensures that we can always have a
 * primary in the case of stepdowns.
 * We also want to avoid changing the votes and priority of the current primary to 0, since this
 * will result in an error.
 *
 * The number of voting nodes in the replica set determines what the config majority is for both
 * reconfig config commitment and reconfig oplog commitment.
 *
 * This function should not throw if everything is working properly.
 */
function reconfigBackground(primary, numNodes) {
    // Calls 'func' with the print() function overridden to be a no-op.
    Random.setRandomSeed();
    const quietly = (func) => {
        const printOriginal = print;
        try {
            print = Function.prototype;
            func();
        } finally {
            print = printOriginal;
        }
    };

    // The stepdown and kill primary hooks run concurrently with this reconfig hook. It is
    // possible that the topology will not be properly updated in time, meaning that the
    // current primary can be undefined if a secondary has not stepped up soon enough.
    if (primary === undefined) {
        jsTestLog("Skipping reconfig because we do not have a primary yet.");
        return {ok: 1};
    }

    jsTestLog("primary is " + primary);

    // Suppress the log messages generated establishing new mongo connections. The
    // run_reconfig_background.js hook is executed frequently by resmoke.py and
    // could lead to generating an overwhelming amount of log messages.
    let conn;
    quietly(() => {
        conn = new Mongo(primary);
    });
    assert.neq(
        null, conn, "Failed to connect to primary '" + primary + "' for background reconfigs");

    var config = conn.getDB("local").system.replset.findOne();

    // Find the correct host in the member config
    const primaryHostIndex = (cfg, pHost) => cfg.members.findIndex(m => m.host === pHost);
    const primaryIndex = primaryHostIndex(config, primary);
    jsTestLog("primaryIndex is " + primaryIndex);

    // Calculate the total number of voting nodes in this set so that we make sure we
    // always have at least two voting nodes. This is so that the primary can always
    // safely step down because there is at least one other electable secondary.
    const numVotingNodes = config.members.filter(member => member.votes === 1).length;

    // Randomly change the vote of a node to 1 or 0 depending on its current value. Do not
    // change the primary's votes.
    var indexToChange = primaryIndex;
    while (indexToChange === primaryIndex) {
        // randInt is exclusive of the upper bound.
        indexToChange = Random.randInt(numNodes);
    }

    jsTestLog("Running reconfig to change votes of node at index" + indexToChange);

    // Change the priority to correspond to the votes. If the member's current votes field
    // is 1, only change it to 0 if there are more than 2 voting members in this set.
    config.version++;
    config.members[indexToChange].votes =
        (config.members[indexToChange].votes === 1 && numVotingNodes > 2) ? 0 : 1;
    config.members[indexToChange].priority = config.members[indexToChange].votes;

    let votingRes = conn.getDB("admin").runCommand({replSetReconfig: config});
    if (!votingRes.ok && !isIgnorableError(votingRes.codeName)) {
        jsTestLog("Reconfig to change votes FAILED.");
        return votingRes;
    }

    return {ok: 1};
}

if (topology.type === Topology.kReplicaSet) {
    var numNodes = topology.nodes.length;
    let res = reconfigBackground(topology.primary, numNodes);
    assert.commandWorked(res, "reconfig hook failed: " + tojson(res));
} else {
    throw new Error('Unsupported topology configuration: ' + tojson(topology));
}
})();
