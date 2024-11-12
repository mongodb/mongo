/**
 * This tests that writes with majority write concern will wait for at least the all durable
 * timestamp to reach the timestamp of the write. This guarantees that once a write is majority
 * committed, reading at the all durable timestamp will read that write.
 *
 * We must disable the test on ephemeral storage engines, because we can't prevent durable from
 * advancing. The means the write succeeds even with journaling and checkpointing turned off.
 *
 * @tags: [requires_persistence]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

function assertWriteConcernTimeout(result) {
    assert.writeErrorWithCode(result, ErrorCodes.WriteConcernTimeout);
    assert(result.hasWriteConcernError(), tojson(result));
    assert(result.getWriteConcernError().errInfo.wtimeout, tojson(result));
}

const rst = new ReplSetTest({
    name: "writes_wait_for_all_durable",
    nodes: 1,
    nodeOptions: {
        // Disable background checkpoints: a zero value disables checkpointing.  This way only
        // journaling can advance the durable timestamp.
        syncdelay: 0
    }
});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const dbName = "test";
const collName = "majority_writes_wait_for_all_durable";
const testDB = primary.getDB(dbName);
const testColl = testDB[collName];

TestData.dbName = dbName;
TestData.collName = collName;

testDB.runCommand({drop: collName, writeConcern: {w: "majority"}});
assert.commandWorked(testDB.createCollection(collName, {writeConcern: {w: "majority"}}));

// Turn off the journal flusher so we don't get durability.
const journalFlusherFP = configureFailPoint(primary, "pauseJournalFlusherBeforeFlush");
journalFlusherFP.wait();

try {
    jsTest.log("Do a write with majority write concern that should time out.");
    assertWriteConcernTimeout(
        testColl.insert({_id: 0}, {writeConcern: {w: "majority", wtimeout: 2 * 1000}}));
} finally {
    journalFlusherFP.off();
}

rst.stopSet();
