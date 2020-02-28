/**
 * This hook runs two reconfig commands against the primary of a replica set:
 * (1): The first reconfig command changes the votes and priority of a node to 0, which will
 * change the voting majority of the set.
 * (2): The second reconfig command runs against the new voting majority and should succeed.
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
const isIgnorableError = function ignorableError(codeName) {
    if (codeName == "ConfigurationInProgress" || codeName == "NotMaster") {
        return true;
    }
    return false;
};

/**
 * Runs two reconfig commands against the primary of a replica set.
 *
 * The first reconfig command randomly chooses a node to change it's votes and priority to 0 or 1
 * based on what the node's current votes and priority fields are. We always check to see that
 * there exists at least one voting node in the set, which ensures that we can always have a
 * primary.
 * We also want to avoid changing the votes and priority of the current primary to 0 to avoid
 * unnecessary stepdowns.
 *
 * The number of voting nodes in the replica set determines what the config majority is for both
 * reconfig config commitment and reconfig oplog commitment.
 *
 * This function should not throw if everything is working properly.
 */
const reconfigBackgroundThread = function reconfigBackground(
    primary, isIgnorableErrorFunc, numNodes, primaryIndex) {
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

    // Suppress the log messages generated establishing new mongo connections. The
    // run_reconfig_background.js hook is executed frequently by resmoke.py and
    // could lead to generating an overwhelming amount of log messages.
    let conn;
    quietly(() => {
        conn = new Mongo(primary);
    });
    assert.neq(
        null, conn, "Failed to connect to primary '" + primary + "' for background reconfigs");

    jsTestLog("Running reconfig to change votes.");

    var config = conn.getDB("local").system.replset.findOne();
    config.version++;

    // Calculate the total number of voting nodes in this set so that we make sure we
    // always have at least one voting node.
    const numVotingNodes = config.members.filter(member => member.votes === 1).length;

    // Randomly change the vote of a node to 1 or 0 depending on its current value. Do not
    // change the primary's votes.
    var indexToChange = primaryIndex;
    while (indexToChange === primaryIndex) {
        // randInt is exclusive of the upper bound.
        indexToChange = Random.randInt(numNodes);
    }

    // Change the priority to correspond to the votes. If the member's current votes field
    // is 1, only change it to 0 if there is another voting member in this set. Otherwise,
    // we risk having no voting members (and no suitable primary).
    config.members[indexToChange].votes = (config.members[indexToChange].votes === 1) ? 0 : 1;
    config.members[indexToChange].priority = config.members[indexToChange].votes;

    let votingRes = conn.getDB("admin").runCommand({replSetReconfig: config});
    if (!votingRes.ok && !isIgnorableErrorFunc(votingRes.codeName)) {
        jsTestLog("Reconfig to change votes FAILED.");
        return votingRes;
    }

    jsTestLog("Running reconfig after changing voting majority.");

    config = conn.getDB("local").system.replset.findOne();
    config.version++;
    let reconfigRes = conn.getDB("admin").runCommand({replSetReconfig: config});
    if (!reconfigRes.ok && !isIgnorableErrorFunc(reconfigRes.codeName)) {
        jsTestLog("Reconfig after changing voting majority FAILED.");
        return reconfigRes;
    }

    return {ok: 1};
};

if (topology.type === Topology.kReplicaSet) {
    var numNodes = topology.nodes.length;
    var primaryIndex = topology.nodes.indexOf(topology.primary);
    const thread = new Thread(
        reconfigBackgroundThread, topology.primary, isIgnorableError, numNodes, primaryIndex);
    try {
        thread.start();
    } finally {
        // Wait for thread to finish and throw an error if it fails.
        let res;
        thread.join();
        res = thread.returnData();

        assert.commandWorked(res, () => "reconfig hook failed: " + tojson(res));
    }
} else {
    throw new Error('Unsupported topology configuration: ' + tojson(topology));
}
})();
