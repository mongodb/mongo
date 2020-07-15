/*
 * Tests that mongos doesn't retry commands with startTransaction=true.
 * @tags: [requires_fcv_44]
 */
(function() {
'use strict';

const setCommandToFail = (nodeConnection, command, namespace) => {
    return nodeConnection.adminCommand({
        configureFailPoint: 'failCommand',
        mode: {times: 1},
        data: {
            errorCode: ErrorCodes.InterruptedDueToReplStateChange,
            failCommands: [command],
            namespace,
            failInternalCommands: true
        }
    });
};

const kDbName = "testDb";
const kCollName = "testColl";
const kNs = `${kDbName}.${kCollName}`;

const kDoc0 = {
    _id: 0
};
const kDoc1 = {
    _id: 1
};

let transactionNumber = 1;

const st = new ShardingTest({
    mongos: 1,
    shards: 1,
    rs: {nodes: 1},
});

// Initializes test and inserts dummy document
jsTest.log("Inserting test document.");
const mongosDB = st.s0.startSession().getDatabase(kDbName);
const primaryConnection = st.rs0.getPrimary();

assert.commandWorked(mongosDB.runCommand({
    insert: kCollName,
    documents: [kDoc0],
}));

// Set the failCommand failpoint to make the next 'find' command fail once due to a failover.
// Start a transaction & execute a find command.
// It should fail once due to the 'failCommand' failpoint and should not be retried.
jsTest.log(
    "Testing that mongos doesn't retry the read command with startTransaction=true on replication set failover.");
assert.commandWorked(setCommandToFail(primaryConnection, "find", kNs));

assert.commandFailedWithCode(mongosDB.runCommand({
    find: kCollName,
    filter: kDoc0,
    startTransaction: true,
    txnNumber: NumberLong(transactionNumber++),
    stmtId: NumberInt(0),
    autocommit: false
}),
                             ErrorCodes.InterruptedDueToReplStateChange);

jsTest.log("Testing that mongos retries retryable writes on failover.");
assert.commandWorked(setCommandToFail(primaryConnection, "insert", kNs));

assert.commandWorked(mongosDB.runCommand(
    {insert: kCollName, documents: [kDoc1], txnNumber: NumberLong(transactionNumber++)}));

st.stop();
})();