/**
 * This hook runs the reconfig command against the primary of a replica set:
 * The reconfig command first chooses a random node (not the primary) and will change
 * its votes and priority to 0 or 1 depending on the current value.
 *
 * This hook will run concurrently with tests.
 */
import {DiscoverTopology, Topology} from "jstests/libs/discover_topology.js";
import newMongoWithRetry from "jstests/libs/retryable_mongo.js";

/**
 * Returns true if the error code is transient.
 */
function isIgnorableError(codeName) {
    return (
        codeName === "ConfigurationInProgress" ||
        codeName === "InterruptedAtShutdown" ||
        codeName === "InterruptedDueToReplStateChange" ||
        codeName === "NodeNotFound" ||
        codeName === "NotWritablePrimary" ||
        codeName === "PrimarySteppedDown" ||
        codeName === "ShutdownInProgress"
    );
}

/**
 * Returns true if the error code indicates the node is currently shutting down.
 */
function isShutdownError(error) {
    // TODO (SERVER-54026): Remove check for error message once the shell correctly
    // propagates the error code.
    return (
        error.code === ErrorCodes.ShutdownInProgress ||
        error.code === ErrorCodes.InterruptedAtShutdown ||
        error.message.includes("The server is in quiesce mode and will shut down") ||
        error.message.includes("interrupted at shutdown")
    );
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

    jsTestLog(`primary is ${primary}`);

    // Suppress the log messages generated establishing new mongo connections. The
    // run_reconfig_background.js hook is executed frequently by resmoke.py and
    // could lead to generating an overwhelming amount of log messages.
    let conn;
    quietly(() => {
        conn = newMongoWithRetry(primary);
    });
    assert.neq(null, conn, `Failed to connect to primary '${primary}' for background reconfigs`);

    let config = assert.commandWorked(conn.getDB("admin").runCommand({replSetGetConfig: 1})).config;

    // Find the correct host in the member config
    const primaryHostIndex = (cfg, pHost) => cfg.members.findIndex((m) => m.host === pHost);
    const primaryIndex = primaryHostIndex(config, primary);
    jsTestLog(`primaryIndex is ${primaryIndex}`);
    jsTestLog(`primary's config: (configVersion: ${config.version}, configTerm: ${config.term})`);

    // Calculate the total number of voting nodes in this set so that we make sure we
    // always have at least two voting nodes. This is so that the primary can always
    // safely step down because there is at least one other electable secondary.
    const numVotingNodes = config.members.filter((member) => member.votes === 1).length;

    // Randomly change the vote of a node to 1 or 0 depending on its current value. Do not
    // change the primary's votes.
    let indexToChange = primaryIndex;
    while (indexToChange === primaryIndex) {
        // randInt is exclusive of the upper bound.
        indexToChange = Random.randInt(numNodes);
    }

    jsTestLog(`Running reconfig to change votes of node at index ${indexToChange}`);

    // Change the priority to correspond to the votes. If the member's current votes field
    // is 1, only change it to 0 if there are more than 3 voting members in this set.
    // We want to ensure that there are at least 3 voting nodes so that killing the primary
    // will not affect a majority.
    config.version++;
    config.members[indexToChange].votes = config.members[indexToChange].votes === 1 && numVotingNodes > 3 ? 0 : 1;
    config.members[indexToChange].priority = config.members[indexToChange].votes;

    let votingRes = conn.getDB("admin").runCommand({replSetReconfig: config});
    jsTestLog(`votingRes: ${tojson(votingRes)}`);
    if (!votingRes.ok && !isIgnorableError(votingRes.codeName)) {
        jsTestLog("Reconfig to change votes FAILED.");
        return votingRes;
    }

    return {ok: 1};
}

// It is possible that the primary will be killed before actually running the reconfig
// command. If we fail with a network error, ignore it.
let res;
try {
    const conn = connect(TestData.connectionString);
    const topology = DiscoverTopology.findConnectedNodes(conn.getMongo());

    if (topology.type !== Topology.kReplicaSet) {
        throw new Error(`Unsupported topology configuration: ${tojson(topology)}`);
    }

    const numNodes = topology.nodes.length;
    res = reconfigBackground(topology.primary, numNodes);
} catch (e) {
    // If the ReplicaSetMonitor cannot find a primary because it has stepped down or
    // been killed, it may take longer than 15 seconds for a new primary to step up.
    // Ignore this error until we find a new primary.
    const kReplicaSetMonitorErrors = [
        /^Could not find host matching read preference.*mode: "primary"/,
        /^can't connect to new replica set primary/,
    ];

    if (isNetworkError(e)) {
        jsTestLog(`Ignoring network error: ${tojson(e)}`);
    } else if (
        kReplicaSetMonitorErrors.some((regex) => {
            return regex.test(e.message);
        })
    ) {
        jsTestLog(`Ignoring replica set monitor error: ${tojson(e)}`);
    } else if (isShutdownError(e)) {
        // It's possible that the primary we passed in gets killed by the kill primary hook.
        // During shutdown, mongod will respond to incoming hello requests with ShutdownInProgress
        // or InterruptedAtShutdown. This hook should ignore both cases and wait until we have a
        // new primary in a subsequent run.
        jsTestLog(`Ignoring ShutdownInProgress error: ${tojson(e)}`);
    } else {
        jsTestLog(`run_reconfig_background unexpected error: ${tojson(e)}`);
        throw e;
    }

    res = {ok: 1};
}

assert.commandWorked(res, `reconfig hook failed: ${tojson(res)}`);
