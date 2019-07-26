// Tests the $out and read concern majority.
(function() {
"use strict";

load("jstests/replsets/rslib.js");           // For startSetIfSupportsReadMajority.
load("jstests/libs/write_concern_util.js");  // For stopReplicationOnSecondaries.

const rst = new ReplSetTest({nodes: 2, nodeOptions: {enableMajorityReadConcern: ""}});

// Skip this test if running with --nojournal and WiredTiger.
if (jsTest.options().noJournal &&
    (!jsTest.options().storageEngine || jsTest.options().storageEngine === "wiredTiger")) {
    print("Skipping test because running WiredTiger without journaling isn't a valid" +
          " replica set configuration");
    return;
}

if (!startSetIfSupportsReadMajority(rst)) {
    jsTestLog("Skipping test since storage engine doesn't support majority read concern.");
    rst.stopSet();
    return;
}

rst.initiate();

const name = "out_majority_read";
const db = rst.getPrimary().getDB(name);

const sourceColl = db.sourceColl;

assert.commandWorked(sourceColl.insert({_id: 1, state: 'before'}));
rst.awaitLastOpCommitted();

stopReplicationOnSecondaries(rst);

// Create the index that is not majority commited
assert.commandWorked(sourceColl.createIndex({state: 1}, {name: "secondIndex"}));

// Run the $out in the parallel shell as it will block in the metadata until the shapshot is
// advanced.
const awaitShell = startParallelShell(`{
        const testDB = db.getSiblingDB("${name}");
        const sourceColl = testDB.sourceColl;

        // Run $out and make sure the {state:1} index is carried over.
        const res = sourceColl.aggregate([{$out: sourceColl.getName()}],
                                         {readConcern: {level: 'majority'}});

        assert.eq(res.itcount(), 0);

        const indexes = sourceColl.getIndexes();
        assert.eq(indexes.length, 2);
        assert.eq(indexes[0].name, "_id_");
        assert.eq(indexes[1].name, "secondIndex");
    }`,
                                          db.getMongo().port);

// Wait for the $out before restarting the replication.
assert.soon(function() {
    const filter = {"command.aggregate": "sourceColl"};
    return assert.commandWorked(db.currentOp(filter)).inprog.length === 1;
});

// Restart data replicaiton and wait until the new write becomes visible.
restartReplicationOnSecondaries(rst);
rst.awaitLastOpCommitted();

awaitShell();

rst.stopSet();
}());
