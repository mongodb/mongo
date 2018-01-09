/**
 * Tests that the transaction table is properly updated on secondaries through oplog replay.
 */
(function() {
    "use strict";

    load("jstests/libs/retryable_writes_util.js");

    if (!RetryableWritesUtil.storageEngineSupportsRetryableWrites(jsTest.options().storageEngine)) {
        jsTestLog("Retryable writes are not supported, skipping test");
        return;
    }

    /**
     * Asserts the connection has a document in its transaction collection that has the given
     * sessionId, txnNumber, and lastWriteOptimeTs.
     */
    function checkTransactionTable(conn, lsid, txnNumber, ts) {
        let table = conn.getDB("config").transactions;
        let res = table.findOne({"_id.id": lsid.id});

        assert.eq(res.txnNum, txnNumber);
        assert.eq(res.lastWriteOpTime.ts, ts);
    }

    /**
     * Asserts the transaction collection document for the given session id is the same on both
     * connections.
     */
    function assertSameRecordOnBothConnections(primary, secondary, lsid) {
        let primaryRecord = primary.getDB("config").transactions.findOne({"_id.id": lsid.id});
        let secondaryRecord = secondary.getDB("config").transactions.findOne({"_id.id": lsid.id});

        assert.eq(bsonWoCompare(primaryRecord, secondaryRecord),
                  0,
                  "expected transaction records: " + tojson(primaryRecord) + " and " +
                      tojson(secondaryRecord) + " to be the same for lsid: " + tojson(lsid));
    }

    /**
     * Runs each command on the primary, awaits replication then asserts the secondary's transaction
     * collection has been updated to store the latest txnNumber and lastWriteOpTimeTs for each
     * sessionId.
     */
    function runCommandsWithDifferentIds(primary, secondary, cmds) {
        // Disable oplog application to ensure the oplog entries come in the same batch.
        secondary.adminCommand({configureFailPoint: "rsSyncApplyStop", mode: "alwaysOn"});

        let responseTimestamps = [];
        cmds.forEach(function(cmd) {
            let res = assert.commandWorked(primary.getDB("test").runCommand(cmd));
            let opTime = (res.opTime.ts ? res.opTime.ts : res.opTime);

            checkTransactionTable(primary, cmd.lsid, cmd.txnNumber, opTime);
            responseTimestamps.push(opTime);
        });

        // After replication, assert the secondary's transaction table has been updated.
        secondary.adminCommand({configureFailPoint: "rsSyncApplyStop", mode: "off"});
        replTest.awaitReplication();
        cmds.forEach(function(cmd, i) {
            checkTransactionTable(secondary, cmd.lsid, cmd.txnNumber, responseTimestamps[i]);
        });

        // Both nodes should have the same transaction collection record for each sessionId.
        cmds.forEach(function(cmd) {
            assertSameRecordOnBothConnections(primary, secondary, cmd.lsid);
        });
    }

    /**
     * Runs each command on the primary and tracks the highest txnNumber and lastWriteOpTimeTs, then
     * asserts the secondary's transaction collection document for the sessionId has been updated
     * correctly.
     */
    function runCommandsWithSameId(primary, secondary, cmds) {
        // Disable oplog application to ensure the oplog entries come in the same batch.
        secondary.adminCommand({configureFailPoint: "rsSyncApplyStop", mode: "alwaysOn"});

        let latestOpTimeTs = Timestamp();
        let highestTxnNumber = NumberLong(-1);
        cmds.forEach(function(cmd) {
            let res = assert.commandWorked(primary.getDB("test").runCommand(cmd));
            let opTime = (res.opTime.ts ? res.opTime.ts : res.opTime);

            checkTransactionTable(primary, cmd.lsid, cmd.txnNumber, opTime);
            latestOpTimeTs = opTime;
            highestTxnNumber =
                (cmd.txnNumber > highestTxnNumber ? cmd.txnNumber : highestTxnNumber);
        });

        // After replication, assert the secondary's transaction table has been updated to store the
        // highest transaction number and the latest write optime.
        secondary.adminCommand({configureFailPoint: "rsSyncApplyStop", mode: "off"});
        replTest.awaitReplication();
        checkTransactionTable(secondary, cmds[0].lsid, highestTxnNumber, latestOpTimeTs);

        // Both nodes should have the same transaction collection record for the sessionId.
        assertSameRecordOnBothConnections(primary, secondary, cmds[0].lsid);
    }

    const replTest = new ReplSetTest({nodes: 2});
    replTest.startSet();
    replTest.initiate();

    let primary = replTest.getPrimary();
    let secondary = replTest.getSecondary();

    ////////////////////////////////////////////////////////////////////////
    // Test insert command

    let insertCmds = [
        {
          insert: "foo",
          documents: [{_id: 10}, {_id: 20}, {_id: 30}, {_id: 40}],
          ordered: true,
          lsid: {id: UUID()},
          txnNumber: NumberLong(5)
        },
        {
          insert: "bar",
          documents: [{_id: 1}, {_id: 2}, {_id: 3}, {_id: 4}],
          ordered: false,
          lsid: {id: UUID()},
          txnNumber: NumberLong(10)
        }
    ];
    runCommandsWithDifferentIds(primary, secondary, insertCmds);

    let lsid = {id: UUID()};
    insertCmds = insertCmds.map(function(cmd) {
        cmd.documents.forEach(function(doc) {
            doc._id = doc._id + 100;
        });
        cmd.lsid = lsid;
        cmd.txnNumber = NumberLong(cmd.txnNumber + 100);
        return cmd;
    });
    runCommandsWithSameId(primary, secondary, insertCmds);

    ////////////////////////////////////////////////////////////////////////
    // Test update command

    let updateCommands = [
        {
          update: "foo",
          updates: [
              {q: {_id: 10}, u: {$set: {x: 10}}, upsert: false},
              {q: {_id: 20}, u: {$set: {x: 20}}, upsert: false},
              {q: {_id: 30}, u: {$set: {x: 30}}, upsert: false},
              {q: {_id: 40}, u: {$set: {x: 40}}, upsert: false}
          ],
          ordered: false,
          lsid: {id: UUID()},
          txnNumber: NumberLong(5)
        },
        {
          update: "bar",
          updates: [
              {q: {_id: 1}, u: {$set: {x: 10}}, upsert: true},
              {q: {_id: 2}, u: {$set: {x: 20}}, upsert: true},
              {q: {_id: 3}, u: {$set: {x: 30}}, upsert: true},
              {q: {_id: 4}, u: {$set: {x: 40}}, upsert: true}
          ],
          ordered: true,
          lsid: {id: UUID()},
          txnNumber: NumberLong(10)
        }
    ];
    runCommandsWithDifferentIds(primary, secondary, updateCommands);

    lsid = {id: UUID()};
    updateCommands = updateCommands.map(function(cmd) {
        cmd.updates.forEach(function(up) {
            up.q._id = up.q._id + 100;
        });
        cmd.lsid = lsid;
        cmd.txnNumber = NumberLong(cmd.txnNumber + 100);
        return cmd;
    });
    runCommandsWithSameId(primary, secondary, updateCommands);

    ////////////////////////////////////////////////////////////////////////
    // Test delete command

    let deleteCommands = [
        {
          delete: "foo",
          deletes: [
              {q: {_id: 10}, limit: 1},
              {q: {_id: 20}, limit: 1},
              {q: {_id: 30}, limit: 1},
              {q: {_id: 40}, limit: 1}
          ],
          ordered: true,
          lsid: {id: UUID()},
          txnNumber: NumberLong(5)
        },
        {
          delete: "bar",
          deletes: [
              {q: {_id: 1}, limit: 1},
              {q: {_id: 2}, limit: 1},
              {q: {_id: 3}, limit: 1},
              {q: {_id: 4}, limit: 1}
          ],
          ordered: false,
          lsid: {id: UUID()},
          txnNumber: NumberLong(10)
        }
    ];
    runCommandsWithDifferentIds(primary, secondary, deleteCommands);

    lsid = {id: UUID()};
    deleteCommands = deleteCommands.map(function(cmd) {
        cmd.deletes.forEach(function(d) {
            d.q._id = d.q._id + 100;
        });
        cmd.lsid = lsid;
        cmd.txnNumber = NumberLong(cmd.txnNumber + 100);
        return cmd;
    });
    runCommandsWithSameId(primary, secondary, deleteCommands);

    replTest.stopSet();
})();
