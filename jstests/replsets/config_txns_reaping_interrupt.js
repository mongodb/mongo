/*
 * Tests that deleting a config.transactions document interrupts all transaction sessions
 * it is associated with.
 *
 * @tags: [uses_transactions]
 */
// This test implicitly write the confif.transactions collection, which is not allowed under a
// session.
TestData.disableImplicitSessions = true;

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {extractUUIDFromObject} from "jstests/libs/uuid_util.js";
import {makeLsidFilter} from "jstests/sharding/libs/sharded_transactions_helpers.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();

const dbName = "testDb";
const collName = "testColl";
const ns = dbName + "." + collName;
const sessionColl = primary.getCollection("config.transactions");

function runInsert(host,
                   lsidUUIDString,
                   lsidTxnNumber,
                   lsidTxnUUIDString,
                   txnNumber,
                   dbName,
                   collName,
                   isRetryableWrite) {
    const conn = new Mongo(host);
    const lsid = {id: UUID(lsidUUIDString)};
    if (lsidTxnNumber) {
        lsid.txnNumber = NumberLong(lsidTxnNumber);
    }
    if (lsidTxnUUIDString) {
        lsid.txnUUID = UUID(lsidTxnUUIDString);
    }
    const cmdObj = {
        insert: collName,
        documents: [{x: 2}],
        lsid,
        txnNumber: NumberLong(txnNumber),

    };
    if (isRetryableWrite || lsid.txnNumber) {
        cmdObj.stmtId = NumberInt(2);
    }
    if (!isRetryableWrite) {
        cmdObj.autocommit = false;
    }
    return conn.getDB(dbName).runCommand(cmdObj);
}

function runTest({committedTxnOpts, inProgressTxnOpts, expectInterrupt}) {
    jsTest.log("Testing " + tojson({committedTxnOpts, inProgressTxnOpts, expectInterrupt}));
    // Start and commit a transaction.
    const cmdObj0 = {
        insert: collName,
        documents: [{x: 0}],
        lsid: committedTxnOpts.lsid,
        txnNumber: NumberLong(committedTxnOpts.txnNumber),
        startTransaction: true,
        autocommit: false,
    };
    if (committedTxnOpts.lsid.txnNumber) {
        cmdObj0.stmtId = NumberInt(0);
    }
    assert.commandWorked(primary.getDB(dbName).runCommand(cmdObj0));
    assert.commandWorked(primary.adminCommand({
        commitTransaction: 1,
        lsid: committedTxnOpts.lsid,
        txnNumber: NumberLong(committedTxnOpts.txnNumber),
        autocommit: false
    }));

    // Start another transaction. Pause it after it has checked out the session.
    const cmdObj1 = {
        insert: collName,
        documents: [{x: 1}],
        lsid: inProgressTxnOpts.lsid,
        txnNumber: NumberLong(inProgressTxnOpts.txnNumber),
    };
    if (inProgressTxnOpts.lsid.txnNumber || inProgressTxnOpts.isRetryableWrite) {
        cmdObj1.stmtId = NumberInt(1);
    }
    if (!inProgressTxnOpts.isRetryableWrite) {
        cmdObj1.startTransaction = true;
        cmdObj1.autocommit = false;
    }
    assert.commandWorked(primary.getDB(dbName).runCommand(cmdObj1));
    const inProgressTxnThread = new Thread(
        runInsert,
        primary.host,
        extractUUIDFromObject(inProgressTxnOpts.lsid.id),
        inProgressTxnOpts.lsid.txnNumber ? inProgressTxnOpts.lsid.txnNumber.toNumber() : null,
        inProgressTxnOpts.lsid.txnUUID ? extractUUIDFromObject(inProgressTxnOpts.lsid.txnUUID)
                                       : null,
        inProgressTxnOpts.txnNumber,
        dbName,
        collName,
        inProgressTxnOpts.isRetryableWrite);
    let fp = configureFailPoint(primary, "hangDuringBatchInsert", {shouldCheckForInterrupt: true});
    inProgressTxnThread.start();

    fp.wait();
    // Delete the config.transactions document for the committed transaction.
    assert.commandWorked(sessionColl.remove(makeLsidFilter(committedTxnOpts.lsid, "_id")));

    fp.off();
    const insertRes = inProgressTxnThread.returnData();
    if (expectInterrupt) {
        assert.commandFailedWithCode(insertRes, ErrorCodes.Interrupted);
    } else {
        assert.commandWorked(insertRes);
        if (!inProgressTxnOpts.isRetryableWrite) {
            assert.commandWorked(primary.adminCommand({
                commitTransaction: 1,
                lsid: inProgressTxnOpts.lsid,
                txnNumber: NumberLong(inProgressTxnOpts.txnNumber),
                autocommit: false
            }));
        }
    }
}

jsTest.log("Test deleting config.transactions document for an external/client session");

{
    const parentLsid = {id: UUID()};
    const parentTxnNumber = 1234;
    runTest({
        committedTxnOpts: {lsid: parentLsid, txnNumber: parentTxnNumber},
        inProgressTxnOpts: {
            lsid: {
                id: parentLsid.id,
                txnUUID: UUID(),
            },
            txnNumber: 1,
        },
        expectInterrupt: true
    });
}

{
    const parentLsid = {id: UUID()};
    const parentTxnNumber = 1234;
    runTest({
        committedTxnOpts: {lsid: parentLsid, txnNumber: parentTxnNumber - 1},
        inProgressTxnOpts: {
            lsid: {
                id: parentLsid.id,
                txnNumber: NumberLong(parentTxnNumber),
                txnUUID: UUID(),
            },
            txnNumber: 1,
        },
        expectInterrupt: true
    });
}

jsTest.log("Test deleting config.transactions document for an internal session for a " +
           "non-retryable write");

{
    const parentLsid = {id: UUID()};
    const parentTxnNumber = 1234;
    runTest({
        committedTxnOpts: {
            lsid: {
                id: parentLsid.id,
                txnUUID: UUID(),
            },
            txnNumber: 1,
        },
        inProgressTxnOpts: {
            lsid: {
                id: parentLsid.id,
            },
            txnNumber: parentTxnNumber,
        },
        expectInterrupt: true
    });
}

{
    const parentLsid = {id: UUID()};
    runTest({
        committedTxnOpts: {
            lsid: {
                id: parentLsid.id,
                txnUUID: UUID(),
            },
            txnNumber: 1,
        },
        inProgressTxnOpts: {
            lsid: {
                id: parentLsid.id,
                txnUUID: UUID(),
            },
            txnNumber: 1,
        },
        expectInterrupt: true
    });
}

jsTest.log("Test deleting config.transactions document for an internal session for the current " +
           "retryable write");

{
    const parentLsid = {id: UUID()};
    const parentTxnNumber = 1234;
    runTest({
        committedTxnOpts: {
            lsid: {
                id: parentLsid.id,
                txnNumber: NumberLong(parentTxnNumber),
                txnUUID: UUID(),
            },
            txnNumber: 1,
        },
        inProgressTxnOpts: {lsid: parentLsid, txnNumber: parentTxnNumber, isRetryableWrite: true},
        expectInterrupt: true
    });
}

{
    const parentLsid = {id: UUID()};
    const parentTxnNumber = 1234;
    runTest({
        committedTxnOpts: {
            lsid: {
                id: parentLsid.id,
                txnNumber: NumberLong(parentTxnNumber),
                txnUUID: UUID(),
            },
            txnNumber: 1,
        },
        inProgressTxnOpts: {
            lsid: {
                id: parentLsid.id,
                txnNumber: NumberLong(parentTxnNumber),
                txnUUID: UUID(),
            },
            txnNumber: 1,
        },
        expectInterrupt: true
    });
}

jsTest.log("Test deleting config.transactions document for an internal transaction for the " +
           "previous retryable write (i.e. no interrupt is expected)");

{
    const parentLsid = {id: UUID()};
    const parentTxnNumber = 1234;
    runTest({
        committedTxnOpts: {
            lsid: {
                id: parentLsid.id,
                txnNumber: NumberLong(parentTxnNumber - 1),
                txnUUID: UUID(),
            },
            txnNumber: 1,
        },
        inProgressTxnOpts: {
            lsid: parentLsid,
            txnNumber: parentTxnNumber,
        },
        expectInterrupt: false
    });
}

{
    const parentLsid = {id: UUID()};
    const parentTxnNumber = 1234;
    runTest({
        committedTxnOpts: {
            lsid: {
                id: parentLsid.id,
                txnNumber: NumberLong(parentTxnNumber - 1),
                txnUUID: UUID(),
            },
            txnNumber: 1,
        },
        inProgressTxnOpts: {
            lsid: parentLsid,
            txnNumber: parentTxnNumber,
            isRetryableWrite: true,
        },
        expectInterrupt: false
    });
}

{
    const parentLsid = {id: UUID()};
    const parentTxnNumber = 1234;
    runTest({
        committedTxnOpts: {
            lsid: {
                id: parentLsid.id,
                txnNumber: NumberLong(parentTxnNumber - 1),
                txnUUID: UUID(),
            },
            txnNumber: 1,
        },
        inProgressTxnOpts: {
            lsid: {
                id: parentLsid.id,
                txnNumber: NumberLong(parentTxnNumber),
                txnUUID: UUID(),
            },
            txnNumber: 1,
        },
        expectInterrupt: false
    });
}

rst.stopSet();