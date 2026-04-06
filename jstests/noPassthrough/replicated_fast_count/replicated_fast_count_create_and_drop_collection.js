/**
 * Tests that a fast count entry is inserted into the fast count store when a collection is created
 * and that this entry is removed when the collection is dropped. Verifies these conditions on both
 * the primary and a secondary, and asserts that the "valid-as-of" timestamps are identical on both
 * nodes. Also tests that inserts to/removes from the fast count store are rolled back if the
 * transaction is aborted.
 *
 * @tags: [
 *   featureFlagReplicatedFastCount,
 *   requires_replication,
 * ]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";

/**
 * Returns all documents in the fast count store.
 * TODO(SERVER-123356): Replace this with a container-level read once a JS API exists.
 */
function readFastCountEntries(conn) {
    return conn.getDB("config").getCollection("fast_count_metadata_store").find().toArray();
}

/**
 * Returns the fast count entry for the given UUID, or undefined if not found.
 */
function findEntry(conn, uuid) {
    return readFastCountEntries(conn).find((e) => bsonWoCompare({a: e._id}, {a: uuid}) === 0);
}

const rst = new ReplSetTest({nodes: [{}, {rsConfig: {priority: 0}}]});
rst.startSet();
rst.initiate();
const primary = rst.getPrimary();
const secondary = rst.getSecondary();

// Creating a collection adds a fast count entry.
const collName = "testcoll";
assert.commandWorked(primary.getDB("testdb").createCollection(collName));

const collInfos = primary.getDB("testdb").getCollectionInfos({name: collName});
assert.eq(collInfos.length, 1);
const collUUID = collInfos[0].info.uuid;

rst.awaitReplication();

const primaryEntryAfterCreate = findEntry(primary, collUUID);
assert(
    primaryEntryAfterCreate,
    `Expected fast count entry for UUID ${collUUID} after create on primary, got: ${tojson(readFastCountEntries(primary))}`,
);
assert.eq(
    primaryEntryAfterCreate.meta.ct,
    0,
    `Expected count=0 after create on primary, got: ${tojson(primaryEntryAfterCreate)}`,
);
assert.eq(
    primaryEntryAfterCreate.meta.sz,
    0,
    `Expected size=0 after create on primary, got: ${tojson(primaryEntryAfterCreate)}`,
);

const secondaryEntryAfterCreate = findEntry(secondary, collUUID);
assert(
    secondaryEntryAfterCreate,
    `Expected fast count entry for UUID ${collUUID} after create on secondary, got: ${tojson(readFastCountEntries(secondary))}`,
);
assert.eq(
    secondaryEntryAfterCreate.meta.ct,
    0,
    `Expected count=0 after create on secondary, got: ${tojson(secondaryEntryAfterCreate)}`,
);
assert.eq(
    secondaryEntryAfterCreate.meta.sz,
    0,
    `Expected size=0 after create on secondary, got: ${tojson(secondaryEntryAfterCreate)}`,
);

assert.eq(
    primaryEntryAfterCreate["valid-as-of"],
    secondaryEntryAfterCreate["valid-as-of"],
    `Expected "valid-as-of" timestamps to match after create. ` +
        `Primary: ${tojson(primaryEntryAfterCreate)}, Secondary: ${tojson(secondaryEntryAfterCreate)}`,
);

// Dropping a collection removes its fast count entry.
assert.commandWorked(primary.getDB("testdb").runCommand({drop: collName}));

rst.awaitReplication();

assert(
    !findEntry(primary, collUUID),
    `Expected no fast count entry for UUID ${collUUID} after drop on primary, got: ${tojson(readFastCountEntries(primary))}`,
);
assert(
    !findEntry(secondary, collUUID),
    `Expected no fast count entry for UUID ${collUUID} after drop on secondary, got: ${tojson(readFastCountEntries(secondary))}`,
);

// Creating a collection inside a committed transaction adds a fast count entry, and the
// "valid-as-of" timestamps match on primary and secondary.
const txnCollName = "txncoll";
const txnSession = primary.startSession();
txnSession.startTransaction();
assert.commandWorked(txnSession.getDatabase("testdb").createCollection(txnCollName));
txnSession.commitTransaction();
txnSession.endSession();

const txnCollInfos = primary.getDB("testdb").getCollectionInfos({name: txnCollName});
assert.eq(txnCollInfos.length, 1);
const txnCollUUID = txnCollInfos[0].info.uuid;

rst.awaitReplication();

const primaryTxnEntry = findEntry(primary, txnCollUUID);
assert(
    primaryTxnEntry,
    `Expected fast count entry for txn-created UUID ${txnCollUUID} on primary, got: ${tojson(readFastCountEntries(primary))}`,
);

const secondaryTxnEntry = findEntry(secondary, txnCollUUID);
assert(
    secondaryTxnEntry,
    `Expected fast count entry for txn-created UUID ${txnCollUUID} on secondary, got: ${tojson(readFastCountEntries(secondary))}`,
);

assert.eq(
    primaryTxnEntry["valid-as-of"],
    secondaryTxnEntry["valid-as-of"],
    `Expected "valid-as-of" timestamps to match for txn-created collection. ` +
        `Primary: ${tojson(primaryTxnEntry)}, Secondary: ${tojson(secondaryTxnEntry)}`,
);

// Creating a collection inside an aborted transaction does not add a fast count entry.
const numEntriesBeforeOnPrimary = readFastCountEntries(primary).length;
const numEntriesBeforeOnSecondary = readFastCountEntries(secondary).length;

const abortSession = primary.startSession();
abortSession.startTransaction();
assert.commandWorked(abortSession.getDatabase("testdb").createCollection("abortedcreatecoll"));
abortSession.abortTransaction();
abortSession.endSession();

rst.awaitReplication();

// The collection should not exist after the abort, so there is no UUID to look up.
// Verify by checking that no new fast count entries were added.
assert.eq(
    readFastCountEntries(primary).length,
    numEntriesBeforeOnPrimary,
    `Expected no new fast count entries on primary after aborted create, got: ${tojson(readFastCountEntries(primary))}`,
);
assert.eq(
    readFastCountEntries(secondary).length,
    numEntriesBeforeOnSecondary,
    `Expected no new fast count entries on secondary after aborted create, got: ${tojson(readFastCountEntries(secondary))}`,
);

rst.stopSet();
