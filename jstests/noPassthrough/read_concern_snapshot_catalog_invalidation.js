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

    function testCommand(cmd, curOpFilter) {
        coll.drop();
        assert.commandWorked(testDB.runCommand({
            createIndexes: kCollName,
            indexes:
                [{key: {haystack: "geoHaystack", a: 1}, name: "haystack_geo", bucketSize: 1}]
        }));
        assert.commandWorked(coll.insert({x: 1}, {writeConcern: {w: "majority"}}));

        // Start a command with readConcern "snapshot" that hangs after establishing a storage
        // engine transaction.
        assert.commandWorked(testDB.adminCommand(
            {configureFailPoint: "hangAfterPreallocateSnapshot", mode: "alwaysOn"}));

        const awaitCommand = startParallelShell(
            "const session = db.getMongo().startSession();" +
                "const sessionDb = session.getDatabase('test');" +
                "session.startTransaction({readConcern: {level: 'snapshot'}});" +
                "const res = sessionDb.runCommand(" + tojson(cmd) + ");" +
                "assert.commandFailedWithCode(res, ErrorCodes.SnapshotUnavailable);" +
                "assert.eq(res.errorLabels, ['TransientTxnError']);" + "session.endSession();",
            rst.ports[0]);

        waitForOp(curOpFilter);

        // Create an index on the collection the command was executed against. This will move the
        // collection's minimum visible timestamp to a point later than the point-in-time referenced
        // by the transaction snapshot.
        assert.commandWorked(testDB.runCommand({
            createIndexes: kCollName,
            indexes: [{key: {x: 1}, name: "x_1"}],
            writeConcern: {w: "majority"}
        }));

        // Disable the hang and check for parallel shell success. Success indicates that the command
        // failed due to collection metadata invalidation.
        assert.commandWorked(
            testDB.adminCommand({configureFailPoint: "hangAfterPreallocateSnapshot", mode: "off"}));

        awaitCommand();
    }

    testCommand({aggregate: kCollName, pipeline: [], cursor: {}},
                {"command.aggregate": kCollName, "command.readConcern.level": "snapshot"});
    testCommand({count: kCollName, filter: {x: 1}},
                {"command.count": kCollName, "command.readConcern.level": "snapshot"});
    testCommand({delete: kCollName, deletes: [{q: {x: 1}, limit: 1}]},
                {"command.delete": kCollName, "command.readConcern.level": "snapshot"});
    testCommand({distinct: kCollName, key: "x"},
                {"command.distinct": kCollName, "command.readConcern.level": "snapshot"});
    testCommand({find: kCollName},
                {"command.find": kCollName, "command.readConcern.level": "snapshot"});
    testCommand({findAndModify: kCollName, query: {x: 1}, remove: true}, {
        "command.findAndModify": kCollName,
        "command.remove": true,
        "command.readConcern.level": "snapshot"
    });
    testCommand({findAndModify: kCollName, query: {x: 1}, update: {$set: {x: 2}}}, {
        "command.findAndModify": kCollName,
        "command.update.$set": {x: 2},
        "command.readConcern.level": "snapshot"
    });
    testCommand({geoSearch: kCollName, near: [0, 0], maxDistance: 1, search: {a: 1}},
                {"command.geoSearch": kCollName, "command.readConcern.level": "snapshot"});
    testCommand({insert: kCollName, documents: [{x: 1}]},
                {"command.insert": kCollName, "command.readConcern.level": "snapshot"});
    testCommand({update: kCollName, updates: [{q: {x: 1}, u: {$set: {x: 2}}}]},
                {"command.update": kCollName, "command.readConcern.level": "snapshot"});

    rst.stopSet();
})();
