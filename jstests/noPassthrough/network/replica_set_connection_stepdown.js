/**
 * Tests that DBClientRS doesn't do any re-targeting on replica set member state changes until it
 * sees a "not master" error response from a command.
 * @tags: [
 *   requires_replication,
 *   grpc_incompatible
 * ]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

const directConn = rst.getPrimary();
const rsConn = new Mongo(rst.getURL());
assert(rsConn.isReplicaSetConnection(), "expected " + rsConn.host + " to be a replica set connection string");

function stepDownPrimary(rst) {
    const awaitShell = startParallelShell(
        () => assert.commandWorked(db.adminCommand({replSetStepDown: 60, force: true})),
        directConn.port,
    );

    // We wait for the primary to transition to the SECONDARY state to ensure we're waiting
    // until after the parallel shell has started the replSetStepDown command and the server is
    // paused at the failpoint. Do not attempt to reconnect to the node, since the node will be
    // holding the global X lock at the failpoint.
    rst.awaitSecondaryNodes(null, [directConn]);

    return awaitShell;
}

const failpoint = "stepdownHangBeforePerformingPostMemberStateUpdateActions";
assert.commandWorked(directConn.adminCommand({configureFailPoint: failpoint, mode: "alwaysOn"}));

const awaitShell = stepDownPrimary(rst);

const error = assert.throws(function () {
    // DBClientRS will continue to send command requests to the node it believed to be primary
    // even after it stepped down so long as it hasn't closed its connection. But this may also
    // throw if the ReplicaSetMonitor's backgroud refresh has already noticed that this node is
    // no longer primary.
    assert.commandFailedWithCode(rsConn.getDB("test").runCommand({find: "mycoll"}), ErrorCodes.NotPrimaryNoSecondaryOk);

    // However, once the server responds back with a "not master" error, DBClientRS will cause
    // the ReplicaSetMonitor to attempt to discover the current primary, which will cause this
    // to definitely throw.
    rsConn.getDB("test").runCommand({find: "mycoll"});
});
assert(
    /Could not find host/.test(error.toString()),
    "find command failed for a reason other than being unable to discover a new primary: " + tojson(error),
);

try {
    assert.commandWorked(directConn.adminCommand({configureFailPoint: failpoint, mode: "off"}));
} catch (e) {
    if (!isNetworkError(e)) {
        throw e;
    }

    // We ignore network errors because it's possible that depending on how quickly the server
    // closes connections that the connection would get closed before the server has a chance to
    // respond to the configureFailPoint command with ok=1.
}

awaitShell();
rst.stopSet();
