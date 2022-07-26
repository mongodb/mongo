/**
 * Tests that the rollback procedure will update the 'config.transactions' table to be consistent
 * with the node data at the 'stableTimestamp', specifically in the case where multiple derived ops
 * to the 'config.transactions' table were coalesced into a single operation during secondary oplog
 * application.
 * We also test that if a node crashes after oplog truncation during rollback, the update made to
 * the 'config.transactions' table is persisted on startup.
 *
 * @tags: [requires_persistence]
 */

(function() {
load("jstests/libs/fail_point_util.js");
load("jstests/libs/write_concern_util.js");

const oplogApplierBatchSize = 100;

function runTest(crashAfterRollbackTruncation) {
    jsTestLog(`Running test with crashAfterRollbackTruncation = ${crashAfterRollbackTruncation}`);
    const rst = new ReplSetTest({
        nodes: {
            n0: {},
            // Set the 'syncdelay' to 1s to speed up checkpointing. Also explicitly set the batch
            // size for oplog application to ensure the number of retryable write statements being
            // made majority committed isn't a multiple of it.
            n1: {syncdelay: 1, setParameter: {replBatchLimitOperations: oplogApplierBatchSize}},
            // Set the bgSyncOplogFetcherBatchSize to 1 oplog entry to guarantee replication
            // progress with the stopReplProducerOnDocument failpoint.
            n2: {setParameter: {bgSyncOplogFetcherBatchSize: 1}},
            n3: {setParameter: {bgSyncOplogFetcherBatchSize: 1}},
            n4: {setParameter: {bgSyncOplogFetcherBatchSize: 1}},
        },
        // Force secondaries to sync from the primary to guarantee replication progress with the
        // stopReplProducerOnDocument failpoint. Also disable primary catchup because some
        // replicated retryable write statements are intentionally not being made majority
        // committed.
        settings: {chainingAllowed: false, catchUpTimeoutMillis: 0},
    });
    rst.startSet();
    rst.initiate();

    const lsid = ({id: UUID()});
    const primary = rst.getPrimary();
    const ns = "test.retryable_write_partial_rollback";
    assert.commandWorked(
        primary.getCollection(ns).insert({_id: 0, counter: 0}, {writeConcern: {w: 5}}));
    // SERVER-65971: Do a write with `lsid` to add an entry to config.transactions. This write will
    // persist after rollback and be updated when the rollback code corrects for omitted writes to
    // the document.
    assert.commandWorked(primary.getCollection(ns).runCommand("insert", {
        documents: [{_id: ObjectId()}],
        lsid,
        txnNumber: NumberLong(1),
        writeConcern: {w: 5},
    }));
    // The default WC is majority and this test can't satisfy majority writes.
    assert.commandWorked(primary.adminCommand(
        {setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}, writeConcern: {w: "majority"}}));
    rst.awaitReplication();

    let [secondary1, secondary2, secondary3, secondary4] = rst.getSecondaries();

    // Disable replication on all of the secondaries to manually control the replication progress.
    const stopReplProducerFailpoints = [secondary1, secondary2, secondary3, secondary4].map(
        conn => configureFailPoint(conn, 'stopReplProducer'));

    // While replication is still entirely disabled, additionally disable replication partway into
    // the retryable write on all but the first secondary. The idea is that while secondary1 will
    // apply all of the oplog entries in a single batch, the other secondaries will only apply up to
    // counterMajorityCommitted oplog entries.
    const counterTotal = oplogApplierBatchSize;
    const counterMajorityCommitted = counterTotal - 2;
    const stopReplProducerOnDocumentFailpoints = [secondary2, secondary3, secondary4].map(
        conn => configureFailPoint(conn,
                                   'stopReplProducerOnDocument',
                                   {document: {"diff.u.counter": counterMajorityCommitted + 1}}));

    assert.commandWorked(primary.getCollection(ns).runCommand("update", {
        updates: Array.from({length: counterTotal}, () => ({q: {_id: 0}, u: {$inc: {counter: 1}}})),
        lsid,
        txnNumber: NumberLong(2),
    }));

    const stmtMajorityCommitted = primary.getCollection("local.oplog.rs")
                                      .findOne({ns, "o.diff.u.counter": counterMajorityCommitted});
    assert.neq(null, stmtMajorityCommitted);

    for (const fp of stopReplProducerFailpoints) {
        fp.off();

        // Wait for the secondary to have applied through the counterMajorityCommitted retryable
        // write statement. We do this for each secondary individually, starting with secondary1, to
        // guarantee that secondary1 will advance its stable_timestamp when learning of the other
        // secondaries also having applied through counterMajorityCommitted.
        assert.soon(() => {
            const {optimes: {appliedOpTime, durableOpTime}} =
                assert.commandWorked(fp.conn.adminCommand({replSetGetStatus: 1}));

            print(`${fp.conn.host}: ${tojsononeline({
                appliedOpTime,
                durableOpTime,
                stmtMajorityCommittedTimestamp: stmtMajorityCommitted.ts
            })}`);

            return bsonWoCompare(appliedOpTime.ts, stmtMajorityCommitted.ts) >= 0 &&
                bsonWoCompare(durableOpTime.ts, stmtMajorityCommitted.ts) >= 0;
        });
    }

    // Wait for secondary1 to have advanced its stable_timestamp.
    assert.soon(() => {
        const {lastStableRecoveryTimestamp} =
            assert.commandWorked(secondary1.adminCommand({replSetGetStatus: 1}));

        print(`${secondary1.host}: ${tojsononeline({
            lastStableRecoveryTimestamp,
            stmtMajorityCommittedTimestamp: stmtMajorityCommitted.ts
        })}`);

        return bsonWoCompare(lastStableRecoveryTimestamp, stmtMajorityCommitted.ts) >= 0;
    });

    // Step up one of the other secondaries and do a write which becomes majority committed to force
    // secondary1 to go into rollback.
    rst.freeze(secondary1);
    let hangAfterTruncate = configureFailPoint(secondary1, 'hangAfterOplogTruncationInRollback');
    assert.commandWorked(secondary2.adminCommand({replSetStepUp: 1}));
    rst.freeze(primary);
    rst.awaitNodesAgreeOnPrimary(undefined, undefined, secondary2);

    for (const fp of stopReplProducerOnDocumentFailpoints) {
        fp.off();
    }

    // Wait for secondary2 to be a writable primary.
    rst.getPrimary();

    // Do a write which becomes majority committed and wait for secondary1 to complete its rollback.
    assert.commandWorked(
        secondary2.getCollection("test.dummy").insert({}, {writeConcern: {w: 'majority'}}));

    // Wait for rollback to finish truncating oplog.
    // Entering rollback will close connections so we expect some network errors while waiting.
    assert.soonNoExcept(() => {
        hangAfterTruncate.wait();
        return true;
    }, `failed to wait for fail point ${hangAfterTruncate.failPointName}`);

    if (crashAfterRollbackTruncation) {
        // Crash the node after it performs oplog truncation.
        rst.stop(secondary1, 9, {allowedExitCode: MongoRunner.EXIT_SIGKILL});
        secondary1 = rst.restart(secondary1, {
            "noReplSet": false,
            setParameter: 'failpoint.stopReplProducer=' + tojson({mode: 'alwaysOn'})
        });
        rst.waitForState(secondary1, ReplSetTest.State.SECONDARY);
        secondary1.setSecondaryOk();
        // On startup, we expect to see the update persisted in the 'config.transactions' table.
        let restoredDoc =
            secondary1.getCollection('config.transactions').findOne({"_id.id": lsid.id});
        assert.neq(null, restoredDoc);
        secondary1.adminCommand({configureFailPoint: "stopReplProducer", mode: "off"});
    } else {
        // Lift the failpoint to let rollback complete and wait for state to change to SECONDARY.
        hangAfterTruncate.off();
        rst.waitForState(secondary1, ReplSetTest.State.SECONDARY);
    }

    // Reconnect to secondary1 after it completes its rollback and step it up to be the new primary.
    rst.awaitNodesAgreeOnPrimary(undefined, undefined, secondary2);
    assert.commandWorked(secondary1.adminCommand({replSetFreeze: 0}));
    rst.stepUp(secondary1);

    const docBeforeRetry = secondary1.getCollection(ns).findOne({_id: 0});
    assert.eq(docBeforeRetry, {_id: 0, counter: counterMajorityCommitted});

    assert.commandWorked(secondary1.getCollection(ns).runCommand("update", {
        updates: Array.from({length: counterTotal}, () => ({q: {_id: 0}, u: {$inc: {counter: 1}}})),
        lsid,
        txnNumber: NumberLong(2),
        writeConcern: {w: 5},
    }));

    const docAfterRetry = secondary1.getCollection(ns).findOne({_id: 0});
    assert.eq(docAfterRetry, {_id: 0, counter: counterTotal});

    rst.stopSet();
}

// Test the general scenario where we perform the appropriate update to the 'config.transactions'
// table during rollback.
runTest(false);
// Extends the test to crash the secondary in the middle of rollback right after oplog truncation.
// We assert that the update made to the 'config.transactions' table persisted on startup.
runTest(true);
})();
