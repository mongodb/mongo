/**
 * Checks that StaleConfig errors for direct shard operations are handled correctly.
 *
 * @tags: [
 *   multiversion_incompatible,
 *   requires_replication,
 * ]
 */
import {withRetryOnTransientTxnError} from "jstests/libs/auto_retry_transaction_in_sharding.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

let rst = new ReplSetTest({
    nodes: 3,
});
rst.startSet({configsvr: ""});
rst.initiate();

let base = 0;
let nextBase = 0;
let primary;
let testDB;
let sessionId;

function setup() {
    base = nextBase;
    nextBase += 5;
    primary = rst.getPrimary();
    testDB = primary.getDB("test");
    sessionId = {id: UUID()};

    for (let i = 0; i < 5; i++) {
        testDB.runCommand({insert: `test${i + base}`, documents: [{x: 1}]});
    }

    // Step-up a new primary so that the first writes for collections test{base + 0-4} will fail
    // with a StaleConfig error. This happens because the new primary will initially not have the
    // sharding metadata for the collections. The metadata will only be recovered after the first
    // failing write for each collection.
    rst.stepUp(rst.getSecondaries()[0]);
    primary = rst.getPrimary();
    testDB = primary.getDB("test");
}

setup();

withRetryOnTransientTxnError(() => {
    // Initial write in a transaction that fails with StaleConfig should internally retry and
    // succeed.
    let command = {
        insert: "test" + (base + 0),
        documents: [{x: 1}],

        lsid: sessionId,
        txnNumber: NumberLong(0),
        autocommit: false,
        startTransaction: true,
    };
    assert.commandWorked(testDB.runCommand(command));

    // Continued writes in a transaction that fails with StaleConfig should fail with the
    // TransientTransactionError label.
    command = {
        insert: "test" + (base + 1),
        documents: [{x: 2}],

        lsid: sessionId,
        txnNumber: NumberLong(0),
        autocommit: false,
    };
    let res = assert.commandFailedWithCode(testDB.runCommand(command), ErrorCodes.StaleConfig);
    assert.contains("TransientTransactionError", res.errorLabels);

    // A retryable write that fails with StaleConfig should fail with the RetryableWriteError label.
    command = {
        insert: "test" + (base + 2),
        documents: [{x: 2}],

        lsid: sessionId,
        txnNumber: NumberLong(1),
    };
    res = assert.commandFailedWithCode(testDB.runCommand(command), ErrorCodes.StaleConfig);
    assert.contains("RetryableWriteError", res.errorLabels);

    // A non-retryable write that fails with StaleConfig should have a StaleConfig writeError and no
    // error labels.
    command = {
        insert: "test" + (base + 3),
        documents: [{x: 2}],
    };
    res = assert.commandFailed(testDB.runCommand(command));
    assert.eq(null, res.errorLabels);
    assert(res.writeErrors);
    assert.eq(res.writeErrors[0].code, ErrorCodes.StaleConfig);

    // A read should succeed even if the collection sharding metadata is not initially available.
    command = {find: "test" + (base + 4), filter: {x: 2}};
    assert.commandWorked(testDB.runCommand(command));
}, setup);

rst.stopSet();
