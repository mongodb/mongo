/**
 * Checks that StaleConfig errors for direct shard operations are handled correctly.
 *
 * @tags: [
 *   multiversion_incompatible,
 *   requires_replication,
 * ]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";

let rst = new ReplSetTest({
    nodes: 3,
});
rst.startSet({configsvr: ""});
rst.initiate();

let primary = rst.getPrimary();
let testDB = primary.getDB("test");
const kSessionId = {
    id: UUID()
};
for (let i = 0; i < 5; i++) {
    testDB.runCommand({insert: `test${i}`, documents: [{x: 1}]});
}

// Step-up a new primary so that the first writes for collections test{0-4} will fail with a
// StaleConfig error. This happens because the new primary will initially not have the sharding
// metadata for the collections. The metadata will only be recovered after the first failing write
// for each collection.
rst.stepUp(rst.getSecondaries()[0]);
primary = rst.getPrimary();
testDB = primary.getDB("test");

// Initial write in a transaction that fails with StaleConfig should internally retry and succeed.
let command = {
    insert: "test0",
    documents: [{x: 1}],

    lsid: kSessionId,
    txnNumber: NumberLong(0),
    autocommit: false,
    startTransaction: true,
};
assert.commandWorked(testDB.runCommand(command));

// Continued writes in a transaction that fails with StaleConfig should fail with the
// TransientTransactionError label.
command = {
    insert: "test1",
    documents: [{x: 2}],

    lsid: kSessionId,
    txnNumber: NumberLong(0),
    autocommit: false,
};
let res = assert.commandFailedWithCode(testDB.runCommand(command), ErrorCodes.StaleConfig);
assert.contains("TransientTransactionError", res.errorLabels);

// A retryable write that fails with StaleConfig should fail with the RetryableWriteError label.
command = {
    insert: "test2",
    documents: [{x: 2}],

    lsid: kSessionId,
    txnNumber: NumberLong(1),
};
res = assert.commandFailedWithCode(testDB.runCommand(command), ErrorCodes.StaleConfig);
assert.contains("RetryableWriteError", res.errorLabels);

// A non-retryable write that fails with StaleConfig should have a StaleConfig writeError and no
// error labels.
command = {
    insert: "test3",
    documents: [{x: 2}],
};
res = assert.commandFailed(testDB.runCommand(command));
assert.eq(null, res.errorLabels);
assert(res.writeErrors);
assert.eq(res.writeErrors[0].code, ErrorCodes.StaleConfig);

// A read should succeed even if the collection sharding metadata is not initially available.
command = {
    find: "test4",
    filter: {x: 2}
};
assert.commandWorked(testDB.runCommand(command));

rst.stopSet();
