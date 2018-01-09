/**
 * Test that the transaction collection can be rolled back properly, as long as the UUID of the
 * collection has not changed between the sync source and the primary.
 *
 * 1. Initiate a 3-node replica set, with two data bearing nodes.
 * 2. Run a transaction on the primary and await replication.
 * 3. Partition the primary.
 * 4. On the partitioned primary:
 *  - Run a transaction with a higher txnNumber for the first session id.
 *  - Run a new transaction for a second session id.
 * 5. On the newly-stepped up primary:
 *  - Run a new transaction for a third session id.
 * 5. Heal the partition.
 * 6. Verify the partitioned primary's transaction collection properly rolled back:
 *  - The txnNumber for the first session id is the original value.
 *  - There is no record for the second session id.
 *  - A record for the third session id was created during oplog replay.
 *
 */
(function() {
    "use strict";

    load("jstests/libs/retryable_writes_util.js");

    if (!RetryableWritesUtil.storageEngineSupportsRetryableWrites(jsTest.options().storageEngine)) {
        jsTestLog("Retryable writes are not supported, skipping test");
        return;
    }

    load("jstests/replsets/rslib.js");

    function assertSameRecordOnBothConnections(primary, secondary, lsid) {
        let primaryRecord = primary.getDB("config").transactions.findOne({"_id.id": lsid.id});
        let secondaryRecord = secondary.getDB("config").transactions.findOne({"_id.id": lsid.id});

        assert.eq(bsonWoCompare(primaryRecord, secondaryRecord),
                  0,
                  "expected transaction records: " + tojson(primaryRecord) + " and " +
                      tojson(secondaryRecord) + " to be the same for lsid: " + tojson(lsid));
    }

    function assertRecordHasTxnNumber(conn, lsid, txnNum) {
        let recordTxnNum = conn.getDB("config").transactions.findOne({"_id.id": lsid.id}).txnNum;
        assert.eq(recordTxnNum,
                  txnNum,
                  "expected node: " + conn + " to have txnNumber: " + txnNum + " for session id: " +
                      lsid + " - instead found: " + recordTxnNum);
    }

    let testName = "rollback_transaction_table";
    let dbName = "test";

    let replTest = new ReplSetTest({
        name: testName,
        nodes: [
            // Primary flops between nodes 0 and 1.
            {},
            {},
            // Arbiter to sway elections.
            {rsConfig: {arbiterOnly: true}}
        ],
        useBridge: true,
    });
    let nodes = replTest.startSet();
    replTest.initiate();

    let downstream = nodes[0];
    let upstream = nodes[1];
    let arbiter = nodes[2];

    jsTestLog("Making sure 'downstream node' is the primary node.");
    assert.eq(downstream, replTest.getPrimary());

    // Renaming or dropping the transactions collection shouldn't crash if command is not rolled
    // back.
    assert.commandWorked(downstream.getDB("config").transactions.renameCollection("foo"));
    assert.commandWorked(downstream.getDB("config").foo.renameCollection("transactions"));
    assert(downstream.getDB("config").transactions.drop());
    assert.commandWorked(downstream.getDB("config").createCollection("transactions"));

    jsTestLog("Running a transaction on the 'downstream node' and waiting for it to replicate.");
    let firstLsid = {id: UUID()};
    let firstCmd = {
        insert: "foo",
        documents: [{_id: 10}, {_id: 30}],
        ordered: false,
        lsid: firstLsid,
        txnNumber: NumberLong(5)
    };

    assert.commandWorked(downstream.getDB(dbName).runCommand(firstCmd));
    replTest.awaitReplication();

    // Both data bearing nodes should have the same record for the first session id.
    assertSameRecordOnBothConnections(downstream, upstream, firstLsid);

    assert.eq(downstream.getDB("config").transactions.find().itcount(), 1);
    assertRecordHasTxnNumber(downstream, firstLsid, NumberLong(5));

    assert.eq(upstream.getDB("config").transactions.find().itcount(), 1);
    assertRecordHasTxnNumber(upstream, firstLsid, NumberLong(5));

    jsTestLog(
        "Creating a partition between 'the downstream and arbiter node' and 'the upstream node.'");
    downstream.disconnect(upstream);
    arbiter.disconnect(upstream);

    jsTestLog(
        "Running a higher transaction for the existing session on only the 'downstream node.'");
    let higherTxnFirstCmd = {
        insert: "foo",
        documents: [{_id: 50}],
        ordered: false,
        lsid: firstLsid,
        txnNumber: NumberLong(20)
    };

    assert.commandWorked(downstream.getDB(dbName).runCommand(higherTxnFirstCmd));

    // Now the data bearing nodes should have different transaction table records for the first
    // session id.
    assert.eq(downstream.getDB("config").transactions.find().itcount(), 1);
    assertRecordHasTxnNumber(downstream, firstLsid, NumberLong(20));

    assert.eq(upstream.getDB("config").transactions.find().itcount(), 1);
    assertRecordHasTxnNumber(upstream, firstLsid, NumberLong(5));

    jsTestLog("Running a transaction for a second session on the 'downstream node.'");
    let secondLsid = {id: UUID()};
    let secondCmd = {
        insert: "foo",
        documents: [{_id: 100}, {_id: 200}],
        ordered: false,
        lsid: secondLsid,
        txnNumber: NumberLong(100)
    };

    assert.commandWorked(downstream.getDB(dbName).runCommand(secondCmd));

    // Only the downstream node should have two transaction table records, one for the first and
    // second session ids.
    assert.eq(downstream.getDB("config").transactions.find().itcount(), 2);
    assertRecordHasTxnNumber(downstream, firstLsid, NumberLong(20));
    assertRecordHasTxnNumber(downstream, secondLsid, NumberLong(100));

    assert.eq(upstream.getDB("config").transactions.find().itcount(), 1);
    assertRecordHasTxnNumber(upstream, firstLsid, NumberLong(5));

    // We do not disconnect the downstream node from the arbiter node at the same time as we
    // disconnect it from the upstream node. This prevents a race where the transaction using the
    // second session id must finish before the downstream node steps down from being the primary.
    jsTestLog(
        "Disconnecting the 'downstream node' from the 'arbiter node' and reconnecting the 'upstream node' to the 'arbiter node.'");
    downstream.disconnect(arbiter);
    upstream.reconnect(arbiter);

    jsTestLog("Waiting for the 'upstream node' to become the new primary.");
    waitForState(downstream, ReplSetTest.State.SECONDARY);
    waitForState(upstream, ReplSetTest.State.PRIMARY);
    assert.eq(upstream, replTest.getPrimary());

    jsTestLog("Running a new transaction for a third session on the 'upstream node.'");
    let thirdLsid = {id: UUID()};
    let thirdCmd = {
        insert: "foo",
        documents: [{_id: 1000}, {_id: 2000}],
        ordered: false,
        lsid: thirdLsid,
        txnNumber: NumberLong(1)
    };

    assert.commandWorked(upstream.getDB(dbName).runCommand(thirdCmd));

    // Now the upstream node also has two transaction table records, but for the first and third
    // session ids, not the first and second.
    assert.eq(downstream.getDB("config").transactions.find().itcount(), 2);
    assertRecordHasTxnNumber(downstream, firstLsid, NumberLong(20));
    assertRecordHasTxnNumber(downstream, secondLsid, NumberLong(100));

    assert.eq(upstream.getDB("config").transactions.find().itcount(), 2);
    assertRecordHasTxnNumber(upstream, firstLsid, NumberLong(5));
    assertRecordHasTxnNumber(upstream, thirdLsid, NumberLong(1));

    // Gets the rollback ID of the downstream node before rollback occurs.
    let downstreamRBIDBefore = assert.commandWorked(downstream.adminCommand('replSetGetRBID')).rbid;

    jsTestLog("Reconnecting the 'downstream node.'");
    downstream.reconnect(upstream);
    downstream.reconnect(arbiter);

    jsTestLog("Waiting for the 'downstream node' to complete rollback.");
    replTest.awaitReplication();
    replTest.awaitSecondaryNodes();

    // Ensure that connection to the downstream node is re-established, since the connection should
    // have gotten killed during the downstream node's transition to ROLLBACK state.
    reconnect(downstream);

    jsTestLog(
        "Checking the rollback ID of the downstream node to confirm that a rollback occurred.");
    assert.neq(downstreamRBIDBefore,
               assert.commandWorked(downstream.adminCommand('replSetGetRBID')).rbid);

    // Verify the record for the first lsid rolled back to its original value, the record for the
    // second lsid was removed, and the record for the third lsid was created during oplog replay.
    jsTestLog("Verifying the transaction collection rolled back properly.");

    assertSameRecordOnBothConnections(downstream, upstream, firstLsid);
    assertRecordHasTxnNumber(downstream, firstLsid, NumberLong(5));
    assertRecordHasTxnNumber(upstream, firstLsid, NumberLong(5));

    assert.isnull(downstream.getDB("config").transactions.findOne({"_id.id": secondLsid.id}));
    assert.isnull(upstream.getDB("config").transactions.findOne({"_id.id": secondLsid.id}));

    assertSameRecordOnBothConnections(downstream, upstream, thirdLsid);
    assertRecordHasTxnNumber(downstream, thirdLsid, NumberLong(1));
    assertRecordHasTxnNumber(upstream, thirdLsid, NumberLong(1));

    assert.eq(downstream.getDB("config").transactions.find().itcount(), 2);
    assert.eq(upstream.getDB("config").transactions.find().itcount(), 2);

    // Confirm the nodes are consistent.
    replTest.checkReplicatedDataHashes(testName);
    replTest.checkOplogs();

    replTest.stopSet();
}());
