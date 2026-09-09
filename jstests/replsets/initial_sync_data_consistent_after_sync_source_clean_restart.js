/**
 * Tests that an initial syncing node stays data consistent when its sync source is cleanly
 * restarted mid-clone.
 *
 * A clean shutdown checkpoints the sync source at its stable timestamp, discarding any writes the
 * initial syncing node may already have read ahead of that timestamp. The attempt therefore cannot
 * carry on against a source that has rolled back behind the attempt's beginApplyingTimestamp: it
 * must abort and retry. The test verifies that exactly one attempt aborts for that reason, that a
 * later attempt succeeds, and that the resulting collection has no missing or duplicate documents.
 *
 * @tags: [
 *   requires_mongobridge,
 *   requires_persistence,
 * ]
 */
import {configureFailPoint, kDefaultWaitForFailPointTimeout} from "jstests/libs/fail_point_util.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

// Using a 5-node replica set is not strictly required but makes it easier to set up the following
// scenario:
//
// 1. All writes eventually becomes majority committed without requiring any client retries.
//
// 2. A read occurs on a node which doesn't yet know the writes have majority committed such that
// rc:local is reading ahead of the stable timestamp.
//
// 3. A clean shutdown occurs on the node which was read from and the checkpoint taken at clean
// shutdown therefore doesn't include any of the writes.
const rst = new ReplSetTest({
    nodes: [
        {},
        {},
        {rsConfig: {priority: 0}},
        {rsConfig: {priority: 0}},
        {rsConfig: {priority: 0}},
    ],
    settings: {chainingAllowed: false},
    useBridge: true,
});

rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const secondary = rst.getSecondaries()[0];

const ns = "test.mycoll";
const collection = primary.getCollection(ns);
assert.commandWorked(collection.createIndex({x: 1}));

// The three priority: 0 nodes which get partitioned off to hold back the stable timestamp. Captured
// before the initial sync node is added so it isn't included.
const laggedNodes = rst.nodes.slice(2);

// Disable majority to avoid advancing stable timestamp.
primary.disconnect(laggedNodes);
secondary.disconnect(laggedNodes);

// Inserting documents which generating a large number of index keys with several threads is a
// reliable to have the record ID assignment order differ from the oplog timestamp assignment order.
// This ensures it is highly likely for replication recovery on startup to choose yet another record
// ID assignment order when applying oplog entries.
async function doInsert(i, host, ns, allThreadsReadyLatch) {
    const conn = new Mongo(host);
    const collection = conn.getCollection(ns);
    allThreadsReadyLatch.countDown();
    allThreadsReadyLatch.await();
    assert.commandWorked(
        collection.insert(
            {_id: i, x: Array.from({length: 10_000}, (_, i) => i)},
            {writeConcern: {w: "majority"}},
        ),
    );
}

const numDocuments = 10;
const allThreadsReadyLatch = new CountDownLatch(numDocuments);
const threads = Array.from(
    {length: numDocuments},
    (_, i) => new Thread(doInsert, i, primary.host, ns, allThreadsReadyLatch),
);

for (const thread of threads) {
    thread.start();
}

assert.soon(() => {
    const collectionToRead = secondary.getCollection(ns);
    const docs = collectionToRead.find({}, {x: 0}).toArray();
    printjson(docs);
    return docs.length === numDocuments;
});

secondary.disconnect(primary);
primary.reconnect(laggedNodes);

for (const thread of threads) {
    thread.join();
}

const afterClusterTime = assert.commandWorked(primary.adminCommand({hello: 1})).operationTime;

const {rollbackId: rollbackIdBefore} = secondary
    .getCollection("local.system.rollback.id")
    .findOne();

// Start new initial sync node, force it to sync from secondary.
const initialSyncNode = rst.add({
    rsConfig: {priority: 0},
    setParameter: {
        "collectionClonerBatchSize": 2,
        "failpoint.forceSyncSourceCandidate": tojson({
            mode: "alwaysOn",
            data: {hostAndPort: secondary.name},
        }),
        "failpoint.initialSyncHangBeforeCreatingOplog": tojson({mode: "alwaysOn"}),
    },
});
rst.reInitiate();

initialSyncNode.adminCommand({
    waitForFailPoint: "initialSyncHangBeforeCreatingOplog",
    timesEntered: 1,
    maxTimeMS: kDefaultWaitForFailPointTimeout,
});
let replSetStatus = assert.commandWorked(initialSyncNode.adminCommand({replSetGetStatus: 1}));
assert.eq(replSetStatus.syncSourceHost, secondary.host);

// Hang at the end of initial sync, before the node clears its progress, so the failed first
// attempt is still visible in replSetGetStatus once the retry has succeeded.
const hangBeforeFinishFp = configureFailPoint(initialSyncNode, "initialSyncHangBeforeFinish");

const hangAfterBatchFp = configureFailPoint(
    initialSyncNode,
    "initialSyncHangCollectionClonerAfterHandlingBatchResponse",
    {nss: ns},
);
initialSyncNode.adminCommand({
    configureFailPoint: "initialSyncHangBeforeCreatingOplog",
    mode: "off",
});
hangAfterBatchFp.wait();

rst.restart(secondary);

// The node's rollback ID won't change from a clean shutdown.
const {rollbackId: rollbackIdAfter} = secondary.getCollection("local.system.rollback.id").findOne();
assert.eq(rollbackIdAfter, rollbackIdBefore);

hangAfterBatchFp.off();

// The sync source restarted mid-clone and rolled back to a checkpoint older than this attempt's
// beginApplyingTimestamp, so the attempt must abort rather than carry on with data that may have
// been rolled back out from under it. The retry then syncs from the same node, which is no longer
// mid-restart, and succeeds.
hangBeforeFinishFp.wait();

const {initialSyncStatus} = assert.commandWorked(
    initialSyncNode.adminCommand({replSetGetStatus: 1}),
);
const attempts = initialSyncStatus.initialSyncAttempts;

// Exactly one attempt aborted for the reason under test. We cannot assert on the total number of
// failed attempts: a retry that starts while the sync source is still coming back up fails with an
// incidental network error, which is legitimate but not what this test is about.
const abortedForCleanShutdown = attempts.filter((attempt) =>
    /cleanly shut down during initial sync/.test(attempt.status),
);
assert.eq(
    abortedForCleanShutdown.length,
    1,
    "expected exactly one attempt to abort because the sync source cleanly shut down",
    {attempts},
);

// ...and the node got there in the end, on a later attempt.
assert.eq(attempts[attempts.length - 1].status, "OK", "expected a later attempt to succeed", {
    attempts,
});

hangBeforeFinishFp.off();

// Heal every partition set up above so the whole set is connected again for the remainder of the
// test and for the consistency checks stopSet() runs.
secondary.reconnect(primary);
secondary.reconnect(laggedNodes);

rst.awaitSecondaryNodes();
rst.awaitReplication();

const initialSyncCollectionToRead = initialSyncNode.getCollection(ns);
const res = assert.commandWorked(
    initialSyncCollectionToRead.runCommand("find", {
        projection: {x: 0},
        hint: {$natural: 1},
    }),
);
const ids = new DBCommandCursor(initialSyncCollectionToRead.getDB(), res).toArray();
assert.eq(
    ids.slice().sort((a, b) => a._id - b._id),
    Array.from({length: numDocuments}, (_, i) => ({_id: i})),
    () =>
        `missing and duplicate documents, actual cursor response $natural order was: ${tojson(
            ids,
        )}`,
);

rst.stopSet();
