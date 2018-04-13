/**
 * This tests that nodes upgrading from 3.6 to 4.0 can recover without data loss if the 4.0 binary
 * crashes before taking a stable checkpoint.
 */
(function() {
    "use strict";

    load('./jstests/multiVersion/libs/multi_rs.js');

    var oldVersion = "3.6";
    var nodes = {n0: {binVersion: oldVersion}, n1: {binVersion: oldVersion}};
    var rst = new ReplSetTest({nodes: nodes});

    rst.startSet();
    rst.initiate();

    function getColl(conn) {
        return conn.getDB("test").getCollection("foo");
    }

    // Insert 100 documents before the upgrade.
    let coll = getColl(rst.getPrimary());
    for (let docNum = 0; docNum < 100; ++docNum) {
        assert.commandWorked(coll.insert({x: docNum}, {writeConcern: {j: true}}));
    }
    rst.awaitReplication(undefined, ReplSetTest.OpTimeType.LAST_DURABLE);

    jsTest.log("Upgrading replica set...");

    rst.upgradeSet({
        binVersion: "latest",  // Only relevant for 3.6 <-> 4.0, but 4.0 does not exist yet.
        setParameter: {logComponentVerbosity: tojsononeline({storage: {recovery: 2}})}
    });

    jsTest.log("Replica set upgraded.");

    // Turn off snapshotting to prevent a stable timestamp from being communicated. Thus
    // preventing any stable checkpoints.
    rst.nodes.forEach(node => assert.commandWorked(node.adminCommand(
                          {configureFailPoint: "disableSnapshotting", mode: "alwaysOn"})));

    // Insert 100 more documents. These should be persisted only in the oplog and not as part of
    // the datafiles in the checkpoint.
    coll = getColl(rst.getPrimary());
    for (let docNum = 100; docNum < 200; ++docNum) {
        assert.commandWorked(coll.insert({x: docNum}, {writeConcern: {j: true}}));
    }

    // Crash the nodes and validate on restart that they both have 200 documents.
    rst.stopSet(9, true, {allowedExitCode: MongoRunner.EXIT_SIGKILL});
    rst.startSet(undefined, true);
    rst.awaitReplication();
    for (let node of rst.nodes) {
        node.setSlaveOk();
        coll = getColl(node);
        assert.eq(200, coll.find().itcount());
    }

    // Upgrade to FCV 4.0 so we persist stable timestamps across clean shutdown.
    assert.commandWorked(rst.getPrimary().adminCommand({setFeatureCompatibilityVersion: "4.0"}));

    // Restart the replica set to load off of a stable checkpoint.
    rst.stopSet(undefined, true);
    rst.startSet(undefined, true);

    // Assert all nodes report a `lastStableCheckpointTimestamp`.
    function getStableCheckpointTimestamp(node) {
        let res = assert.commandWorked(node.adminCommand("replSetGetStatus"));
        if (!res.hasOwnProperty("lastStableCheckpointTimestamp")) {
            return Timestamp(0, 0);
        }

        return res.lastStableCheckpointTimestamp;
    }
    rst.nodes.forEach(node => assert.gt(getStableCheckpointTimestamp(node), Timestamp(0, 0)));
    rst.stopSet();
})();
