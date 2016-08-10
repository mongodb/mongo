// Test the catch-up behavior of new primaries.

load("jstests/replsets/rslib.js");

(function() {

    "use strict";
    var name = "catch_up";
    var rst = new ReplSetTest({name: name, nodes: 3, useBridge: true});

    rst.startSet();
    var conf = rst.getReplSetConfig();
    conf.settings = {
        heartbeatIntervalMillis: 500,
        electionTimeoutMillis: 10000,
        catchUpTimeoutMillis: 10000
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

    function enableFailPoint(node) {
        jsTest.log("enable failpoint " + node.host);
        // Disable syncing on both secondaries.
        assert.commandWorked(
            node.adminCommand({configureFailPoint: 'pauseRsBgSyncProducer', mode: 'alwaysOn'}),
            'Failed to configure pauseRsBgSyncProducer failpoint.');
        assert.commandWorked(
            node.adminCommand({configureFailPoint: 'stopOplogFetcher', mode: 'alwaysOn'}));
    }

    function disableFailPoint(node) {
        jsTest.log("disable failpoint " + node.host);
        assert.commandWorked(
            node.adminCommand({configureFailPoint: 'stopOplogFetcher', mode: 'off'}));
        try {
            assert.commandWorked(
                node.adminCommand({configureFailPoint: 'pauseRsBgSyncProducer', mode: 'off'}),
                'Failed to disable pauseRsBgSyncProducer failpoint.');
        } catch (ex) {
            // Enable bgsync producer may cause rollback, which will close all connections
            // including the one sending "configureFailPoint".
            print("got exception when disabling fail point 'pauseRsBgSyncProducer': " + e);
        }
    }

    function stepUp(node) {
        assert.commandWorked(node.adminCommand({replSetStepUp: 1}));
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
        assert.eq(oplog.count(op), count, "oplog: " + tojson(oplogArray));
    }

    function isEarlierTimestamp(ts1, ts2) {
        if (ts1.getTime() == ts2.getTime()) {
            return ts1.getInc() < ts2.getInc();
        }
        return ts1.getTime() < ts2.getTime();
    }

    function countMatchedLog(node, regex) {
        var res = node.adminCommand({getLog: 'global'});
        assert.commandWorked(res);
        var count = 0;
        res.log.forEach(function(line) {
            if (regex.exec(line)) {
                count += 1;
            }
        });
        return count;
    }

    jsTest.log("Case 1: The primary is up-to-date after freshness scan.");
    // Should complete transition to primary immediately.
    rst.awaitReplication(30000, ReplSetTest.OpTimeType.LAST_DURABLE);
    var newPrimary = stepUp(rst.getSecondary());
    rst.waitForState(newPrimary, ReplSetTest.State.PRIMARY, 1000);
    // Should win an election and finish the transition very quickly.
    assert.eq(newPrimary, rst.getPrimary());

    jsTest.log("Case 2: The primary needs to catch up, succeeds in time.");
    rst.awaitReplication(30000, ReplSetTest.OpTimeType.LAST_DURABLE);
    // Write documents that cannot be replicated to secondaries in time.
    rst.getSecondaries().forEach(enableFailPoint);
    doWrites(rst.getPrimary());
    var latestOp = getLatestOp(rst.getPrimary());
    // New primary wins immediately, but needs to catch up.
    newPrimary = stepUp(rst.getSecondary());
    rst.waitForState(newPrimary, ReplSetTest.State.PRIMARY, 1000);
    // Check this node is not writable.
    assert.eq(newPrimary.getDB("test").isMaster().ismaster, false);
    // Disable fail point to allow replication.
    rst.nodes.forEach(disableFailPoint);
    // getPrimary() blocks until the primary finishes drain mode.
    assert.eq(newPrimary, rst.getPrimary());
    // Wait for all secondaries to catch up
    rst.awaitReplication();
    // Check the latest op on old primary is preserved on the new one.
    checkOpInOplog(newPrimary, latestOp, 1);

    jsTest.log("Case 3: The primary needs to catch up, fails due to timeout.");
    rst.awaitReplication(30000, ReplSetTest.OpTimeType.LAST_DURABLE);
    // Write documents that cannot be replicated to secondaries in time.
    rst.getSecondaries().forEach(enableFailPoint);
    doWrites(rst.getPrimary());
    latestOp = getLatestOp(rst.getPrimary());

    // New primary wins immediately, but needs to catch up.
    newPrimary = stepUp(rst.getSecondary());
    rst.waitForState(newPrimary, ReplSetTest.State.PRIMARY, 1000);
    var latestOpOnNewPrimary = getLatestOp(newPrimary);
    // Wait until the new primary completes the transition to primary and writes a no-op.
    assert.soon(function() {
        return countMatchedLog(newPrimary, /Cannot catch up oplog after becoming primary/) > 0;
    });
    disableFailPoint(newPrimary);
    assert.eq(newPrimary, rst.getPrimary());

    // Wait for the no-op "new primary" after winning an election, so that we know it has
    // finished transition to primary.
    assert.soon(function() {
        return isEarlierTimestamp(latestOpOnNewPrimary.ts, getLatestOp(newPrimary).ts);
    });
    // The extra oplog entries on the old primary are not replicated to the new one.
    checkOpInOplog(newPrimary, latestOp, 0);
    rst.getSecondaries().forEach(disableFailPoint);

    jsTest.log("Case 4: The primary needs to catch up, but has to change sync source to catch up.");
    rst.awaitReplication(30000, ReplSetTest.OpTimeType.LAST_DURABLE);
    // Write documents that cannot be replicated to secondaries in time.
    rst.getSecondaries().forEach(enableFailPoint);
    doWrites(rst.getPrimary());
    var oldPrimary = rst.getPrimary();
    var oldSecondaries = rst.getSecondaries();
    latestOp = getLatestOp(oldPrimary);
    newPrimary = stepUp(oldSecondaries[0]);
    rst.waitForState(newPrimary, ReplSetTest.State.PRIMARY, 1000);
    // Disable fail point on one of the other secondaries.
    // Wait until it catches up with the old primary.
    disableFailPoint(oldSecondaries[1]);
    awaitOpTime(oldSecondaries[1], latestOp.ts);
    // Disconnect the new primary and the old one.
    oldPrimary.disconnect(newPrimary);
    // Disable the failpoint, the new primary should sync from the other secondary.
    disableFailPoint(newPrimary);
    assert.eq(newPrimary, rst.getPrimary());
    checkOpInOplog(newPrimary, latestOp, 1);

})();
