/*
 * Tests that reaping expired internal transaction sessions does not cause the operations on the
 * corresponding logical sessions to be interrupted.
 *
 * @tags: [requires_fcv_60, uses_transactions]
 */
(function() {
"use strict";

const logicalSessionRefreshMillis = 1000;
const rst = new ReplSetTest({
    nodes: 2,
    nodeOptions: {
        setParameter: {
            // Disable the TTL monitor to ensure that the config.system.sessions entry for the
            // test session is always around.
            ttlMonitorEnabled: false,
            disableLogicalSessionCacheRefresh: false,
            TransactionRecordMinimumLifetimeMinutes: 0,
            logicalSessionRefreshMillis
        }
    }
});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();

const dbName = "testDb";
const collName = "testColl";
const minReapTimes = 5;
const minDuration = minReapTimes * logicalSessionRefreshMillis;

const sessionsColl = primary.getCollection("config.system.sessions");

function runTest(isRetryableWriteSession, runTxns) {
    jsTest.log(`Start testing with ${tojson({isRetryableWriteSession, runTxns})}`);
    const session = primary.startSession({retryWrites: isRetryableWriteSession});
    const db = session.getDatabase(dbName);
    const coll = db.getCollection(collName);

    assert.commandWorked(coll.remove({}));
    assert.commandWorked(primary.adminCommand({refreshLogicalSessionCacheNow: 1}));
    assert.eq(1, sessionsColl.find({"_id.id": session.getSessionId().id}).itcount());

    const startTime = new Date();
    let currTime = new Date();

    while (currTime - startTime < minDuration) {
        const isTxn = runTxns && Math.random() > 0.5;
        if (isTxn) {
            session.startTransaction();
        }

        const doc = {_id: UUID()};
        const insertOp = {
            insert: collName,
            documents: [doc],
        };
        if (isRetryableWriteSession && !isTxn) {
            insertOp.stmtId = NumberInt(1);
        }
        assert.commandWorked(db.adminCommand(
            {testInternalTransactions: 1, commandInfos: [{dbName: dbName, command: insertOp}]}));

        if (isTxn) {
            assert.commandWorked(session.commitTransaction_forTesting());
        }
        assert.eq(coll.find(doc).itcount(), 1);
        currTime = new Date();
    }

    const endTime = new Date();
    jsTest.log(`Finished testing with ${
        tojson({isRetryableWriteSession, timeTaken: (endTime - startTime)})}`);
}

for (let isRetryableWriteSession of [true, false]) {
    for (let runTxns of [true, false]) {
        runTest(isRetryableWriteSession, runTxns);
    }
}

rst.stopSet();
})();
