/**
 * Tests that DBClientRS doesn't do any re-targeting on replica set member state changes until it
 * sees a "not master" error response from a command.
 */
(function() {
    "use strict";

    const rst = new ReplSetTest({nodes: 1});
    rst.startSet();
    rst.initiate();

    const directConn = rst.getPrimary();
    const rsConn = new Mongo(rst.getURL());
    assert(rsConn.isReplicaSetConnection(),
           "expected " + rsConn.host + " to be a replica set connection string");

    function stepDownPrimary(rst) {
        const awaitShell = startParallelShell(function() {
            const error = assert.throws(function() {
                const res = db.adminCommand({replSetStepDown: 60, force: true});
                print("replSetStepDown did not throw exception but returned: " + tojson(res));
            });
            assert(isNetworkError(error),
                   "replSetStepDown did not disconnect client; failed with " + tojson(error));
        }, directConn.port);

        // We wait for the primary to transition to the SECONDARY state to ensure we're waiting
        // until after the parallel shell has started the replSetStepDown command and the server is
        // paused at the failpoint.
        rst.waitForState(directConn, ReplSetTest.State.SECONDARY);

        return awaitShell;
    }

    const failpoint = "stepdownHangBeforePerformingPostMemberStateUpdateActions";
    assert.commandWorked(
        directConn.adminCommand({configureFailPoint: failpoint, mode: "alwaysOn"}));

    const awaitShell = stepDownPrimary(rst);

    // DBClientRS will continue to send command requests to the node it believed to be primary even
    // after it stepped down so long as it hasn't closed its connection.
    assert.commandFailedWithCode(rsConn.getDB("test").runCommand({find: "mycoll"}),
                                 ErrorCodes.NotMasterNoSlaveOk);

    // However, once the server responds back with a "not master" error, DBClientRS will cause the
    // ReplicaSetMonitor to attempt to discover the current primary.
    const error = assert.throws(function() {
        rsConn.getDB("test").runCommand({find: "mycoll"});
    });
    assert(/Could not find host/.test(error.toString()),
           "find command failed for a reason other than being unable to discover a new primary: " +
               tojson(error));

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
})();
