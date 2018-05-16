// Test that getMore for a majority read on a secondary only reads committed data.
(function() {
    "use strict";

    const name = "read_concern_majority_getmore_secondaries";
    const replSet = new ReplSetTest({
        name: name,
        nodes:
            [{}, {rsConfig: {priority: 0}}, {rsConfig: {priority: 0}}, {rsConfig: {priority: 0}}],
        settings: {chainingAllowed: false}
    });
    replSet.startSet();
    replSet.initiate();

    function stopDataReplication(node) {
        jsTest.log("Stop data replication on " + node.host);
        assert.commandWorked(
            node.adminCommand({configureFailPoint: "rsSyncApplyStop", mode: "alwaysOn"}));
    }

    function startDataReplication(node) {
        jsTest.log("Start data replication on " + node.host);
        assert.commandWorked(
            node.adminCommand({configureFailPoint: "rsSyncApplyStop", mode: "off"}));
    }

    const dbName = "test";
    const collName = "coll";

    const primary = replSet.getPrimary();
    const secondaries = replSet.getSecondaries();
    const secondary = secondaries[0];

    const primaryDB = primary.getDB(dbName);
    const secondaryDB = secondary.getDB(dbName);

    // Insert data on primary and allow it to become committed.
    for (let i = 0; i < 4; i++) {
        assert.commandWorked(primaryDB[collName].insert({_id: i}));
    }

    // Await commit.
    replSet.awaitReplication();
    replSet.awaitLastOpCommitted();

    // Stop data replication on 2 secondaries to prevent writes being committed.
    stopDataReplication(secondaries[1]);
    stopDataReplication(secondaries[2]);

    // Write more data to primary.
    for (let i = 4; i < 8; i++) {
        assert.commandWorked(primaryDB[collName].insert({_id: i}, {writeConcern: {w: 2}}));
    }

    // Check that it reached the secondary.
    assert.docEq([{_id: 0}, {_id: 1}, {_id: 2}, {_id: 3}, {_id: 4}, {_id: 5}, {_id: 6}, {_id: 7}],
                 secondaryDB[collName].find().sort({_id: 1}).toArray());

    // It is important that this query does not do an in-memory sort. Otherwise the initial find
    // will consume all of the results from the storage engine in order to sort them, so we will not
    // be testing that the getMore does not read uncommitted data from the storage engine.
    let res = primaryDB[collName].find().sort({_id: 1}).batchSize(2).readConcern("majority");
    assert.docEq([{_id: 0}, {_id: 1}, {_id: 2}, {_id: 3}], res.toArray());

    // Similarly, this query must not do an in-memory sort.
    res = secondaryDB[collName].find().sort({_id: 1}).batchSize(2).readConcern("majority");
    assert.docEq([{_id: 0}, {_id: 1}, {_id: 2}, {_id: 3}], res.toArray());

    // Disable failpoints and shutdown.
    replSet.getSecondaries().forEach(startDataReplication);
    replSet.stopSet();
}());
