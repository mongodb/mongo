/**
 * Test that consistency checks for preimage work as expected. Consistency is defined by performing
 * these steps:
 *   * Fix a nsUUID to scan the preimage collection
 *   * Obtain all preimage entries of the namespace by sorting in descending order of '_id.ts' and
 *     '_id.applyIndexOps'.
 *   * For each entry position and node:
 *     * The entry exists in the node at that position and is equal across all nodes in that
 *       position.
 *     * The entry doesn't exist in the node at that position.
 * @tags: [
 *   requires_replication,
 * ]
 */

(function() {
"use strict";

function getPreImage(collectionIndex, ts) {
    return {
        _id: {
            nsUUID: UUID(`3b241101-e2bb-4255-8caf-4136c566a12${collectionIndex}`),
            ts: new Timestamp(ts, 0),
            applyOpsIndex: 0,
        },
        operationTime: ISODate("9000-01-01"),  // Put a date really far off into the future.
    };
}

assert.doesNotThrow(() => {
    const replSetTest = new ReplSetTest({name: "replSet", nodes: 2});
    replSetTest.startSet();
    replSetTest.initiate();

    const primary = replSetTest.getPrimary();
    const secondary = replSetTest.getSecondary();

    const coll = primary.getDB("config")["system.preimages"];
    const secondaryColl = secondary.getDB("config")["system.preimages"];

    // Insert documents to the preimages collection. Ensure they are not replicated to secondaries.
    coll.insert(getPreImage(1, 0));
    coll.insert(getPreImage(1, 1));
    coll.insert(getPreImage(2, 1));
    coll.insert(getPreImage(3, 1));

    assert.eq(coll.find({}).itcount(), 4);
    assert.eq(secondaryColl.find({}).itcount(), 0);

    // Now insert preimages in the old secondary.
    replSetTest.stepUp(secondary);

    const newPrimary = replSetTest.getPrimary();
    const newColl = newPrimary.getDB("config")["system.preimages"];
    newColl.insert(getPreImage(1, 1));
    newColl.insert(getPreImage(2, 1));

    // Verify that even if the data isn't consistent the test passes as consistency is defined as
    // two nodes having entries equal or non-existent starting from the end.
    replSetTest.stopSet();
});

const replSetTest = new ReplSetTest({name: "replSet", nodes: 2});
replSetTest.startSet();
replSetTest.initiate();

const primary = replSetTest.getPrimary();
const secondary = replSetTest.getSecondary();

const coll = primary.getDB("config")["system.preimages"];
const secondaryColl = secondary.getDB("config")["system.preimages"];

// Insert a document to the preimage collection. Ensure it is not replicated to secondaries.
coll.insert(getPreImage(1, 0));
assert.eq(coll.find({}).itcount(), 1);
assert.eq(secondaryColl.find({}).itcount(), 0);

// Now insert another document to the secondary, this will cause an inconsistency error when we stop
// the replica set.
replSetTest.stepUp(secondary);

const newPrimary = replSetTest.getPrimary();

const newColl = newPrimary.getDB("config")["system.preimages"];
newColl.insert(getPreImage(1, 1));

// Verify that the two nodes are inconsistent.
assert.throws(() => replSetTest.stopSet());

try {
    replSetTest.stopSet();
} catch (e) {
    // Verify that the inconsistency is the one we're looking for in preimages.
    assert.eq(e.message.includes("Detected preimage entries that have different content"), true);
}
// Tear down the nodes now without checking for consistency.
replSetTest.stopSet(undefined, undefined, {skipCheckDBHashes: true});
})();
