/**
 * Test that verifies that time waiting for a ticket is logged as part of slow query logging.
 * Induces queueing for ticket by exhausting read tickets.
 */

// Set the number of tickets to a small value to force ticket exhaustion.
const kNumReadTickets = 5;
const kNumDocs = 1000;
const kQueuedReaders = 20;

const rst = new ReplSetTest({
    nodes: 1,
    nodeOptions: {
        setParameter: {
            storageEngineConcurrentReadTransactions: kNumReadTickets,
            // Make yielding more common.
            internalQueryExecYieldPeriodMS: 1,
            internalQueryExecYieldIterations: 1,
            featureFlagLogSlowOpsBasedOnTimeWorking: true,
        },
    }
});
rst.startSet();
rst.initiate();

const dbName = jsTestName();
const collName = "testcoll";
const primary = rst.getPrimary();
const db = primary.getDB(dbName);
const coll = db[collName];
let queuedReaders = [];

// Lower slowMs threshold to 1 to ensure all operations that wait at least 1ms for a ticket are
// logged.
assert.commandWorked(db.setProfilingLevel(1, {slowms: 1}));

jsTestLog("Fill collection [" + dbName + "." + collName + "] with " + kNumDocs + " docs");
for (let i = 0; i < kNumDocs; i++) {
    assert.commandWorked(coll.insert({"x": i}));
}

// Kick off many parallel readers that perform long collection scans that are subject to yields. Use
// jsTestName().timing_coordination' collection to coordinate timing between the main thread of the
// test and all the readers.
TestData.dbName = dbName;
TestData.collName = collName;
for (TestData.id = 0; TestData.id < kQueuedReaders; TestData.id++) {
    queuedReaders.push(startParallelShell(function() {
        db.getMongo().setSecondaryOk();
        db.getSiblingDB(TestData.dbName)
            .timing_coordination.insert({_id: TestData.id, msg: "queued reader started"});
        while (
            db.getSiblingDB(TestData.dbName).timing_coordination.findOne({_id: "stop reading"}) ==
            null) {
            db.getSiblingDB(TestData.dbName)[TestData.collName].aggregate([{"$count": "x"}]);
        }
    }, primary.port));
    jsTestLog("queued reader " + queuedReaders.length + " initiated");
}

jsTestLog("Wait for many parallel reader shells to run");
assert.soon(() => db.getSiblingDB(TestData.dbName).timing_coordination.count({
    msg: "queued reader started"
}) >= kQueuedReaders,
            "Expected at least " + kQueuedReaders + " queued readers to start.");

// Ticket exhaustion should force remaining readers to wait in queue for a ticket.
jsTestLog("Wait for no available read tickets");
assert.soon(() => {
    let stats = db.runCommand({serverStatus: 1});
    jsTestLog(stats.admission.execution);
    return stats.admission.execution.read.available == 0;
}, "Expected to have no available read tickets.");

jsTestLog("Stop readers and clean up");
assert.commandWorked(db.getSiblingDB(dbName).timing_coordination.insertOne({_id: "stop reading"}));

for (let i = 0; i < queuedReaders.length; i++) {
    queuedReaders[i]();
}

const predicate = new RegExp(`Slow query.*"${coll}.*"ticketWaitMillis"`);
assert(checkLog.checkContainsOnce(primary, predicate),
       "Could not find log containing " + predicate);

rst.stopSet();
