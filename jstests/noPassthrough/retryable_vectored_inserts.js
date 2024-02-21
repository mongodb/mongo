/**
 * Test that vectored inserts can be retried, including after a failover and/or restart.
 * @tags: [requires_persistence]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {extractUUIDFromObject} from "jstests/libs/uuid_util.js";
import {reconnect} from "jstests/replsets/rslib.js";

// Set up a two-node replica set
const testName = TestData.testName;
const replTest = new ReplSetTest({
    name: testName,
    nodes: 2,
    nodeOptions: {setParameter: {internalInsertMaxBatchSize: 3}},
    settings: {chainingAllowed: false}
});
replTest.startSet();
replTest.initiateWithHighElectionTimeout();

const dbName = testName;
const primary = replTest.getPrimary();
const primaryDB = primary.getDB(dbName);
const secondary = replTest.getSecondary();
const secondaryDB = secondary.getDB(dbName);
const collName1 = "testcoll1";
const collName2 = "testcoll2";
const bbCollName = "bbtestcoll";

const singleBatchLSID = {
    id: UUID()
};
const singleBatchCommand = {
    insert: collName1,
    lsid: singleBatchLSID,
    txnNumber: NumberLong(0),
    documents: [{_id: "s1"}, {_id: "s2"}, {_id: "s3"}]
};
// First try
assert.commandWorked(primaryDB.runCommand(singleBatchCommand));
// Retry on primary
assert.commandWorked(primaryDB.runCommand(singleBatchCommand));

const multiBatchLSID = {
    id: UUID()
};
const multiBatchCommand = {
    insert: collName2,
    lsid: multiBatchLSID,
    txnNumber: NumberLong(0),
    documents: [{_id: "m1"}, {_id: "m2"}, {_id: "m3"}, {_id: "m4"}, {_id: "m5"}, {_id: "m6"}]
};
// First try
assert.commandWorked(primaryDB.runCommand(multiBatchCommand));
// Retry on primary
assert.commandWorked(primaryDB.runCommand(multiBatchCommand));

// Make sure we can retry when the command failed somewhere in the middle.
const brokenBatchLSID = {
    id: UUID()
};
const brokenBatchCommandNoSession = {
    insert: bbCollName,
    documents: [{_id: "b1"}, {_id: "b2"}, {_id: "b3"}, {_id: "b4"}, {_id: "b5"}, {_id: "b6"}]
};
const brokenBatchCommand =
    Object.merge(brokenBatchCommandNoSession, {lsid: brokenBatchLSID, txnNumber: NumberLong(0)});
const brokenBatchFirstBatch = [{_id: "b1"}, {_id: "b2"}, {_id: "b3"}];

jsTestLog("Starting a vectored insert blocked between batches");
let failPoint =
    configureFailPoint(primaryDB,
                       "hangAfterCollectionInserts",
                       {collectionNS: primaryDB[bbCollName].getFullName(), first_id: "b4"});
let insertThread = new Thread(
    (host, dbName, collName, brokenBatchCommandNoSession, sessuuid) => {
        const db = new Mongo(host).getDB(dbName);
        const brokenBatchCommand = Object.merge(
            brokenBatchCommandNoSession, {lsid: {id: UUID(sessuuid)}, txnNumber: NumberLong(0)});
        assert.commandFailedWithCode(db.runCommand(brokenBatchCommand),
                                     ErrorCodes.InterruptedDueToReplStateChange);
    },
    primary.host,
    dbName,
    bbCollName,
    brokenBatchCommandNoSession,
    extractUUIDFromObject(brokenBatchLSID.id));
insertThread.start();
failPoint.wait();

assert.soon(() =>
                bsonUnorderedFieldsCompare(primaryDB[bbCollName].find({}).sort({_id: 1}).toArray(),
                                           brokenBatchFirstBatch) == 0);
assert.soon(
    () => bsonUnorderedFieldsCompare(secondaryDB[bbCollName].find({}).sort({_id: 1}).toArray(),
                                     brokenBatchFirstBatch) == 0);

// Failover
assert.commandWorked(secondary.adminCommand({replSetStepUp: 1}));
assert.eq(secondary, replTest.getPrimary());

// Release failpoint; second insert batch should fail now.
failPoint.off();
insertThread.join();

function checkRetries() {
    const retryNode = replTest.getPrimary();
    const retryDB = retryNode.getDB(dbName);

    // Retry single batch command on new primary and make sure documents match after.
    jsTestLog("Retrying single batch insert.");
    assert.commandWorked(retryDB.runCommand(singleBatchCommand));
    assert.docEq(secondaryDB[singleBatchCommand.insert].find({}).sort({_id: 1}).toArray(),
                 singleBatchCommand.documents)
    assert.docEq(primaryDB[singleBatchCommand.insert].find({}).sort({_id: 1}).toArray(),
                 singleBatchCommand.documents)

    // Retry multi batch command on new primary and make sure documents match after.
    jsTestLog("Retrying multi batch insert.");
    assert.commandWorked(retryDB.runCommand(multiBatchCommand));
    assert.docEq(secondaryDB[multiBatchCommand.insert].find({}).sort({_id: 1}).toArray(),
                 multiBatchCommand.documents)
    assert.docEq(primaryDB[multiBatchCommand.insert].find({}).sort({_id: 1}).toArray(),
                 multiBatchCommand.documents)

    // Retry broken batch command on new primary and ensure the whole insert worked.
    jsTestLog("Retrying broken batch insert.");
    assert.commandWorked(retryDB.runCommand(brokenBatchCommand));
    assert.docEq(secondaryDB[brokenBatchCommand.insert].find({}).sort({_id: 1}).toArray(),
                 brokenBatchCommand.documents)
    // Since this didn't work the first time, we need to await replication for it to work on the old
    // primary.
    if (retryNode == secondary) {
        replTest.awaitReplication();
    }
    assert.docEq(primaryDB[brokenBatchCommand.insert].find({}).sort({_id: 1}).toArray(),
                 brokenBatchCommand.documents);
}

// The new primary (old secondary) should only have the first batch.
assert.docEq(secondaryDB[bbCollName].find({}).sort({_id: 1}).toArray(), brokenBatchFirstBatch);
jsTestLog("Doing retries on new primary.");
checkRetries();

// Now restart the original primary and step it back up
jsTestLog("Restarting original primary");
replTest.restart(primary);
reconnect(primary);
replTest.awaitSecondaryNodes();

jsTestLog("Failing over to original primary.");
assert.commandWorked(primary.adminCommand({replSetStepUp: 1}));
assert.eq(primary, replTest.getPrimary());

jsTestLog("Doing retries on original primary");
checkRetries();

replTest.stopSet();
