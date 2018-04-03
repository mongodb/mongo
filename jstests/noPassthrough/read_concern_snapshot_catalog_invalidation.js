// Tests that snapshot reads return an error when accessing a collection whose metadata is invalid
// for the snapshot's point in time.
// @tags: [requires_replication]
(function() {
    "use strict";

    const kDbName = "test";
    const kCollName = "coll";

    const rst = new ReplSetTest({nodes: 1});
    rst.startSet();
    rst.initiate();

    const testDB = rst.getPrimary().getDB(kDbName);
    if (!testDB.serverStatus().storageEngine.supportsSnapshotReadConcern) {
        rst.stopSet();
        return;
    }
    const adminDB = testDB.getSiblingDB("admin");
    const coll = testDB.getCollection(kCollName);
    coll.drop();

    function waitForOp(curOpFilter) {
        assert.soon(
            function() {
                const res = adminDB.aggregate([{$currentOp: {}}, {$match: curOpFilter}]).toArray();
                if (res.length === 1) {
                    return true;
                }
                return false;
            },
            function() {
                return "Failed to find operation in $currentOp output: " +
                    tojson(adminDB.aggregate([{$currentOp: {}}]).toArray());
            });
    }

    assert.commandWorked(coll.insert({x: 1}, {writeConcern: {w: "majority"}}));

    // Start a snapshot find that hangs after establishing a storage engine transaction.
    assert.commandWorked(testDB.adminCommand(
        {configureFailPoint: "hangAfterPreallocateSnapshot", mode: "alwaysOn"}));

    const awaitCommand = startParallelShell(function() {
        const session = db.getMongo().startSession();
        const sessionDb = session.getDatabase("test");
        session.startTransaction({readConcern: {level: "snapshot"}});
        const res = sessionDb.runCommand({find: "coll"});
        assert.commandFailedWithCode(res, ErrorCodes.SnapshotUnavailable);
        session.endSession();
    }, rst.ports[0]);

    waitForOp({"command.find": kCollName, "command.readConcern.level": "snapshot"});

    // Create an index on the collection the find was executed against. This will move the
    // collection's minimum visible timestamp to a point later than the point-in-time referenced
    // by the find snapshot.
    assert.commandWorked(testDB.runCommand({
        createIndexes: kCollName,
        indexes: [{key: {x: 1}, name: "x_1"}],
        writeConcern: {w: "majority"}
    }));

    // Disable the hang and check for parallel shell success. Success indicates that the find
    // command failed due to collection metadata invalidation.
    assert.commandWorked(
        testDB.adminCommand({configureFailPoint: "hangAfterPreallocateSnapshot", mode: "off"}));

    awaitCommand();

    rst.stopSet();
})();
