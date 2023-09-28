/**
 * Test that consistency checks for change_collection(s) work as expected. Consistency is defined by
 * performing these steps:
 *   * For each tenant's change_collection, open a reverse cursor on each node.
 *   * For each entry position and node:
 *     * The entry exists in the node at that position and is equal across all nodes in that
 *       position.
 *     * The entry doesn't exist in the node at that position.
 * @tags: [
 *   requires_replication,
 * ]
 */

import {
    ChangeStreamMultitenantReplicaSetTest
} from "jstests/serverless/libs/change_collection_util.js";

function getChangeCollectionEntry(ts) {
    const farOffDate = ISODate("2100-01-01");
    const epochSeconds = farOffDate.valueOf() / 1000;
    // Return a document inserted with a date really far off into the future.
    return {
        _id: new Timestamp(epochSeconds, ts),
    };
}

const kTenantId = ObjectId();

assert.doesNotThrow(() => {
    const replSetTest = new ChangeStreamMultitenantReplicaSetTest({
        name: "replSet",
        nodes: 2,
        nodeOptions: {setParameter: {disableExpiredChangeCollectionRemover: true}},
    });

    const primary = ChangeStreamMultitenantReplicaSetTest.getTenantConnection(
        replSetTest.getPrimary().host, kTenantId, kTenantId.str);
    replSetTest.awaitReplication();  // Await user creation replication.

    const secondary = ChangeStreamMultitenantReplicaSetTest.getTenantConnection(
        replSetTest.getSecondary().host, kTenantId, kTenantId.str);
    const primaryChangeCollection = primary.getDB("config")["system.change_collection"];
    const secondaryChangeCollection = secondary.getDB("config")["system.change_collection"];

    // Insert documents to the change_collection ensure they are not replicated to secondaries.
    primaryChangeCollection.insert(getChangeCollectionEntry(0));
    primaryChangeCollection.insert(getChangeCollectionEntry(1));
    primaryChangeCollection.insert(getChangeCollectionEntry(2));
    primaryChangeCollection.insert(getChangeCollectionEntry(3));

    replSetTest.awaitReplication();
    assert.eq(primaryChangeCollection.find({}).itcount(), 4);
    assert.eq(secondaryChangeCollection.find({}).itcount(), 0);

    // Now insert documents in the old secondary.
    replSetTest.stepUp(replSetTest.getSecondary());

    const newPrimary = ChangeStreamMultitenantReplicaSetTest.getTenantConnection(
        replSetTest.getPrimary().host, kTenantId, kTenantId.str);
    const newPrimaryChangeCollection = newPrimary.getDB("config")["system.change_collection"];
    newPrimaryChangeCollection.insert(getChangeCollectionEntry(2));
    newPrimaryChangeCollection.insert(getChangeCollectionEntry(3));

    // Verify that even if the data isn't consistent the test passes as consistency is defined as
    // two nodes having entries equal or non-existent starting from the end.
    replSetTest.stopSet();
});

const replSetTest = new ChangeStreamMultitenantReplicaSetTest({
    name: "replSet",
    nodes: 2,
    nodeOptions: {setParameter: {disableExpiredChangeCollectionRemover: true}},
});

const primary = ChangeStreamMultitenantReplicaSetTest.getTenantConnection(
    replSetTest.getPrimary().host, kTenantId, kTenantId.str);
replSetTest.awaitReplication();  // Await user creation replication.

const secondary = ChangeStreamMultitenantReplicaSetTest.getTenantConnection(
    replSetTest.getSecondary().host, kTenantId, kTenantId.str);
const primaryChangeCollection = primary.getDB("config")["system.change_collection"];
const secondaryChangeCollection = secondary.getDB("config")["system.change_collection"];

// // Insert a few documents to the change_collection. Ensure it is not replicated to secondaries.
primaryChangeCollection.insert(getChangeCollectionEntry(0));
primaryChangeCollection.insert(getChangeCollectionEntry(1));
primaryChangeCollection.insert(getChangeCollectionEntry(2));
primaryChangeCollection.insert(getChangeCollectionEntry(3));
primaryChangeCollection.insert(getChangeCollectionEntry(4));
primaryChangeCollection.insert(getChangeCollectionEntry(5));
primaryChangeCollection.insert(getChangeCollectionEntry(6));
primaryChangeCollection.insert(getChangeCollectionEntry(7));
assert.eq(primaryChangeCollection.find({}).itcount(), 8);
assert.eq(secondaryChangeCollection.find({}).itcount(), 0);

// Now insert another set of documents to the secondary, this will cause an inconsistency error
// when we stop the replica set.
replSetTest.stepUp(replSetTest.getSecondary());

const newPrimary = ChangeStreamMultitenantReplicaSetTest.getTenantConnection(
    replSetTest.getPrimary().host, kTenantId, kTenantId.str);
const newPrimaryChangeCollection = newPrimary.getDB("config")["system.change_collection"];

newPrimaryChangeCollection.insert(getChangeCollectionEntry(0));
newPrimaryChangeCollection.insert(getChangeCollectionEntry(1));
newPrimaryChangeCollection.insert(getChangeCollectionEntry(2));
newPrimaryChangeCollection.insert(getChangeCollectionEntry(3));
newPrimaryChangeCollection.insert(getChangeCollectionEntry(5));
newPrimaryChangeCollection.insert(getChangeCollectionEntry(6));
newPrimaryChangeCollection.insert(getChangeCollectionEntry(7));

// Verify that the two nodes are inconsistent.
let inconsistencyFound = false;
try {
    replSetTest.stopSet();
} catch (e) {
    // Verify that the inconsistency is the one we're looking for in change_collection.
    assert.eq(e.message.includes("system.change_collection"), true);
    inconsistencyFound = true;
}

assert(inconsistencyFound, "Expected an inconsistency to be found in change_collection");

// Tear down the nodes now without checking for consistency.
replSetTest.stopSet(undefined, undefined, {skipCheckDBHashes: true});
