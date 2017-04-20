// Test the catch-up behavior of new primaries.

(function() {
    "use strict";

    load("jstests/libs/check_log.js");
    load("jstests/libs/write_concern_util.js");
    load("jstests/replsets/rslib.js");

    var name = "catch_up";
    var rst = new ReplSetTest({name: name, nodes: 3, useBridge: true});

    rst.startSet();
    var conf = rst.getReplSetConfig();
    conf.settings = {
        heartbeatIntervalMillis: 500,
        electionTimeoutMillis: 10000,
        catchUpTimeoutMillis: 4 * 60 * 1000
    };
    rst.initiate(conf);
    rst.awaitSecondaryNodes();

    var primary = rst.getPrimary();
    var primaryColl = primary.getDB("test").coll;

    // Set verbosity for replication on all nodes.
    var verbosity = {
        "setParameter": 1,
        "logComponentVerbosity": {
            "replication": {"verbosity": 2},
        }
    };
    rst.nodes.forEach(function(node) {
        node.adminCommand(verbosity);
    });

    function stepUp(node) {
        assert.soon(function() {
            node.adminCommand({replSetStepUp: 1});
            return node.adminCommand('replSetGetStatus').myState == ReplSetTest.State.PRIMARY;
        });

        return node;
    }

    function doWrites(node) {
        for (var i = 0; i < 3; i++) {
            assert.writeOK(node.getDB("test").foo.insert({x: i}));
        }
    }

    function checkOpInOplog(node, op, count) {
        node.getDB("admin").getMongo().setSlaveOk();
        var oplog = node.getDB("local")['oplog.rs'];
        var oplogArray = oplog.find().toArray();
        assert.eq(oplog.count(op), count, "op: " + tojson(op) + ", oplog: " + tojson(oplogArray));
    }

    function isEarlierTimestamp(ts1, ts2) {
        if (ts1.getTime() == ts2.getTime()) {
            return ts1.getInc() < ts2.getInc();
        }
        return ts1.getTime() < ts2.getTime();
    }

    rst.awaitReplication(ReplSetTest.kDefaultTimeoutMS, ReplSetTest.OpTimeType.LAST_DURABLE);

    jsTest.log("Case 1: The primary is up-to-date after freshness scan.");
    // Should complete transition to primary immediately.
    var newPrimary = stepUp(rst.getSecondary());
    rst.awaitNodesAgreeOnPrimary();
    // Should win an election and finish the transition very quickly.
    assert.eq(newPrimary, rst.getPrimary());
    rst.awaitReplication(ReplSetTest.kDefaultTimeoutMS, ReplSetTest.OpTimeType.LAST_DURABLE);

    jsTest.log("Case 2: The primary needs to catch up, succeeds in time.");
    // Write documents that cannot be replicated to secondaries in time.
    var originalSecondaries = rst.getSecondaries();
    stopServerReplication(originalSecondaries);
    doWrites(rst.getPrimary());
    var latestOp = getLatestOp(rst.getPrimary());
    // New primary wins immediately, but needs to catch up.
    newPrimary = stepUp(rst.getSecondary());
    rst.awaitNodesAgreeOnPrimary();
    // Check this node is not writable.
    assert.eq(newPrimary.getDB("test").isMaster().ismaster, false);
    // Disable fail point to allow replication.
    restartServerReplication(originalSecondaries);
    // getPrimary() blocks until the primary finishes drain mode.
    assert.eq(newPrimary, rst.getPrimary());
    // Wait for all secondaries to catch up
    rst.awaitReplication();
    // Check the latest op on old primary is preserved on the new one.
    checkOpInOplog(newPrimary, latestOp, 1);
    rst.awaitReplication(ReplSetTest.kDefaultTimeoutMS, ReplSetTest.OpTimeType.LAST_DURABLE);

    jsTest.log("Case 3: The primary needs to catch up, but has to change sync source to catch up.");
    // Write documents that cannot be replicated to secondaries in time.
    stopServerReplication(rst.getSecondaries());
    doWrites(rst.getPrimary());
    var oldPrimary = rst.getPrimary();
    originalSecondaries = rst.getSecondaries();
    latestOp = getLatestOp(oldPrimary);
    newPrimary = stepUp(originalSecondaries[0]);
    rst.awaitNodesAgreeOnPrimary();
    // Disable fail point on one of the other secondaries.
    // Wait until it catches up with the old primary.
    restartServerReplication(originalSecondaries[1]);
    assert.commandWorked(originalSecondaries[1].adminCommand({replSetSyncFrom: oldPrimary.host}));
    awaitOpTime(originalSecondaries[1], latestOp.ts);
    // Disconnect the new primary and the old one.
    oldPrimary.disconnect(newPrimary);
    // Disable the failpoint, the new primary should sync from the other secondary.
    restartServerReplication(newPrimary);
    assert.eq(newPrimary, rst.getPrimary());
    checkOpInOplog(newPrimary, latestOp, 1);
    // Restore the broken connection
    oldPrimary.reconnect(newPrimary);
    rst.awaitReplication(ReplSetTest.kDefaultTimeoutMS, ReplSetTest.OpTimeType.LAST_DURABLE);

    jsTest.log("Case 4: The primary needs to catch up, fails due to timeout.");
    // Reconfigure replicaset to decrease catchup timeout
    conf = rst.getReplSetConfigFromNode();
    conf.version++;
    conf.settings.catchUpTimeoutMillis = 10 * 1000;
    reconfig(rst, conf);
    rst.awaitReplication(ReplSetTest.kDefaultTimeoutMS, ReplSetTest.OpTimeType.LAST_DURABLE);
    rst.awaitNodesAgreeOnPrimary();

    // Write documents that cannot be replicated to secondaries in time.
    originalSecondaries = rst.getSecondaries();
    stopServerReplication(originalSecondaries);
    doWrites(rst.getPrimary());
    latestOp = getLatestOp(rst.getPrimary());

    // New primary wins immediately, but needs to catch up.
    newPrimary = stepUp(originalSecondaries[0]);
    rst.awaitNodesAgreeOnPrimary();
    var latestOpOnNewPrimary = getLatestOp(newPrimary);
    // Wait until the new primary completes the transition to primary and writes a no-op.
    checkLog.contains(newPrimary, "Cannot catch up oplog after becoming primary");
    restartServerReplication(newPrimary);
    assert.eq(newPrimary, rst.getPrimary());

    // Wait for the no-op "new primary" after winning an election, so that we know it has
    // finished transition to primary.
    assert.soon(function() {
        return isEarlierTimestamp(latestOpOnNewPrimary.ts, getLatestOp(newPrimary).ts);
    });
    // The extra oplog entries on the old primary are not replicated to the new one.
    checkOpInOplog(newPrimary, latestOp, 0);
    restartServerReplication(originalSecondaries[1]);
})();
