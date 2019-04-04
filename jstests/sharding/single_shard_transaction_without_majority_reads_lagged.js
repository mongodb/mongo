/**
 * Test that single-shard transactions succeeed against replica sets whose primary has
 * 'enableMajorityReadConcern':false and whose secondary is significantly lagged.
 *
 * With majority reads disabled, we are not guaranteed to be able to service reads at the majority
 * commit point. We can only provide reads within a window behind the primary's 'lastApplied'. The
 * size of that window is controlled by 'maxTargetSnapshotHistoryWindowInSeconds', which is 5
 * seconds by default. If the commit point lag is greater than that amount, reading at that time
 * fails with a SnapshotTooOld error. Therefore, in order for the transaction to succeed, merizos
 * needs to pick a read timestamp that is not derived from the commit point, but rather from the
 * 'lastApplied' optime on the primary.
 *
 * @tags: [uses_transactions, requires_find_command]
 */

(function() {
    "use strict";

    load("jstests/libs/write_concern_util.js");  // for 'stopServerReplication' and
                                                 // 'restartServerReplication'.

    const name = "single_shard_transaction_without_majority_reads_lagged";
    const dbName = "test";
    const collName = name;

    const shardingTest = new ShardingTest({
        shards: 1,
        rs: {
            nodes: [
                {/* primary */ enableMajorityReadConcern: false},
                {/* secondary */ rsConfig: {priority: 0}}
            ]
        }
    });

    const rst = shardingTest.rs0;
    const merizos = shardingTest.s;
    const merizosDB = merizos.getDB(dbName);
    const merizosColl = merizosDB[collName];

    // Create and shard collection beforehand.
    assert.commandWorked(merizosDB.adminCommand({enableSharding: merizosDB.getName()}));
    assert.commandWorked(
        merizosDB.adminCommand({shardCollection: merizosColl.getFullName(), key: {_id: 1}}));

    // This is the last write the secondary will have before the start of the transaction.
    assert.commandWorked(merizosColl.insert({_id: 1, x: 10}, {writeConcern: {w: "majority"}}));

    // We want the secondary to lag for an amount generously greater than the history window.
    const secondary = rst.getSecondary();
    const maxWindowResult = assert.commandWorked(secondary.getDB("admin").runCommand(
        {"getParameter": 1, "maxTargetSnapshotHistoryWindowInSeconds": 1}));
    stopServerReplication(secondary);

    const maxWindowInMS = maxWindowResult.maxTargetSnapshotHistoryWindowInSeconds * 1000;
    const lagTimeMS = maxWindowInMS * 2;
    const startTime = Date.now();
    let nextId = 1000;

    // Insert a stream of writes to the primary with _ids all numbers greater or equal than
    // 1000 (this is done to easily distinguish them from the write above done with _id: 1).
    // The secondary cannot replicate them, so this has the effect of making that node lagged.
    // It would also update merizos' notion of the latest clusterTime in the system.
    while (Date.now() - startTime < maxWindowInMS) {
        assert.commandWorked(merizosColl.insert({id: nextId}));
        nextId++;
        sleep(50);
    }

    // This is an update only the primary has. The test will explicitly check for it in a few lines.
    assert.commandWorked(merizosColl.update({_id: 1, x: 10}, {_id: 1, x: 20}));

    const session = merizos.startSession();
    const sessionDB = session.getDatabase(dbName);
    const sessionColl = sessionDB.getCollection(collName);

    // Begin a transaction and make sure its associated read succeeds. To make this test stricter,
    // have the transaction manipulate data that differs between the primary and secondary.
    session.startTransaction();
    assert.commandWorked(sessionColl.update({_id: 1}, {$inc: {x: 1}}));

    session.commitTransaction();

    // Confirm that the results of the transaction are based on what the primary's data was when we
    // started the transaction.
    assert.eq(21, sessionColl.findOne({_id: 1}).x);

    restartServerReplication(secondary);
    shardingTest.stop();
})();
