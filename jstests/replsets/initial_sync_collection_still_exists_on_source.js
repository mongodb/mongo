/**
 * Tests that initial sync does not silently skip a collection whose creation is commit-pending on
 * the sync source.
 *
 * There is a window during createCollection where the collection is durably committed (its oplog
 * entry is written, so it can be at or below the syncing node's 'beginApplyingTimestamp') but not
 * yet published to the in-memory collection catalog. In that window listCollections includes the
 * commit-pending collection, but by-UUID resolution (used by count and listIndexes during command
 * authorization) returned NamespaceNotFound, which the cloner used to interpret as "dropped on
 * source" and skip the collection. Since the create is at or below 'beginApplyingTimestamp', oplog
 * replay never recreated it either, so the node would finish initial sync permanently missing the
 * collection and later fassert applying a steady-state DDL on it.
 *
 * This test reproduces the race organically by hanging a createCollection at the
 * 'hangBeforePublishingCatalogUpdates' failpoint (durable but unpublished) while a new node runs
 * initial sync.
 *
 * @tags: [
 *   requires_replication,
 *   multiversion_incompatible,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {checkLog} from "src/mongo/shell/check_log.js";

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const dbName = "test";
const collName = "commit_pending_coll";
const primaryDB = primary.getDB(dbName);

// Ensure the database exists before hanging a collection creation in it.
assert.commandWorked(primaryDB.createCollection("dummy"));

// Hang the createCollection after it is durably committed (oplog entry written) but before the
// collection is published to the in-memory collection catalog. In this window listCollections
// returns the commit-pending collection but by-UUID lookups failed with NamespaceNotFound.
const failPoint = configureFailPoint(primary, "hangBeforePublishingCatalogUpdates", {
    collectionNS: dbName + "." + collName,
});
const awaitCreate = startParallelShell(
    `assert.commandWorked(db.getSiblingDB('${dbName}').runCommand({create: '${collName}'}));`,
    primary.port,
);
failPoint.wait();

// Add a new node that will perform initial sync from the primary. Its 'beginApplyingTimestamp'
// will be at or above the durable create, its listCollections will see the commit-pending
// collection, and its cloner's by-UUID reads would get NamespaceNotFound.
const newNode = rst.add({
    rsConfig: {priority: 0, votes: 0},
    setParameter: {
        logComponentVerbosity: tojson({replication: {initialSync: 2}}),
    },
});
rst.reInitiate();

// Given that the fix makes the count command wait for commit pending to be published,
// synchronizing test expectations is not possible without intrusive instrumentation.
// This is a best effort way of maintaining reproducibility. At worst some slow runs would
// fail to reproduce in case of regression, but most of the time it will work.
checkLog.containsJson(newNode, 21148, {namespace: "test.dummy"});
checkLog.containsJson(newNode, 21069, {cloner: "CollectionCloner", stage: "count"});
sleep(5_000);

// Turn off failpoint to allow both "create" and initial sync to finish.
failPoint.off();
awaitCreate();

rst.awaitSecondaryNodes(null, [newNode]);
rst.awaitReplication();

// The collection must exist on the initial-synced node.
newNode.setSecondaryOk();
assert.eq(
    1,
    newNode.getDB(dbName).getCollectionInfos({name: collName}).length,
    "collection was not cloned onto the initial sync node",
);

// Full data-consistency check between the sync source and the initial-synced node.
rst.checkReplicatedDataHashes();

rst.stopSet();
