/**
 * Tests that a snapshot read at a timestamp after a collection drop returns no results, even when
 * the drop has been committed to durable storage but not yet published to the in-memory catalog.
 *
 * This is a regression test for SERVER-96916: establishConsistentCollection did not check pending
 * commits when a readTimestamp was provided, causing PIT reads to use the stale in-memory
 * Collection and return documents from a dropped collection.
 *
 * @tags: [
 *   requires_replication,
 *   requires_snapshot_read,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const rst = new ReplSetTest({nodes: 2});
rst.startSet();
rst.initiate();
const primary = rst.getPrimary();
const testDB = primary.getDB("test");
const collName = "dropcoll";

// Insert documents and wait for replication so the collection is at a stable timestamp.
assert.commandWorked(testDB[collName].insert([{x: 1}, {x: 2}, {x: 3}]));
rst.awaitReplication();

// Hang the drop after durable commit but before publishing to the in-memory catalog.
const fp = configureFailPoint(primary, "hangBeforePublishingCatalogUpdates", {
    collectionNS: testDB[collName].getFullName(),
});

const awaitDrop = startParallelShell(
    `assert.commandWorked(db.getSiblingDB('test').runCommand({drop: '${collName}'}))`,
    primary.port,
);
fp.wait();

// Advance the cluster time past the drop's durable commit by doing another write.
assert.commandWorked(testDB.other.insert({y: 1}, {writeConcern: {w: "majority"}}));
const clusterTime = testDB.getSession().getOperationTime();

// Snapshot read while the drop is pending: the collection is dropped in durable at clusterTime,
// so the read must return no results even though the in-memory catalog still has the collection.
const resDuringPending = assert.commandWorked(
    testDB.runCommand({find: collName, readConcern: {level: "snapshot", atClusterTime: clusterTime}}),
);
const docsDuringPending = new DBCommandCursor(testDB, resDuringPending).toArray();
assert.eq(
    docsDuringPending.length,
    0,
    "Expected no documents from a dropped collection during pending commit, got: " + tojson(docsDuringPending),
);

// Let the drop publish to the in-memory catalog.
fp.off();
awaitDrop();

// Same snapshot read after publish: must also return no results.
const resAfterPublish = assert.commandWorked(
    testDB.runCommand({find: collName, readConcern: {level: "snapshot", atClusterTime: clusterTime}}),
);
const docsAfterPublish = new DBCommandCursor(testDB, resAfterPublish).toArray();
assert.eq(
    docsAfterPublish.length,
    0,
    "Expected no documents from a dropped collection after publish, got: " + tojson(docsAfterPublish),
);

rst.stopSet();
