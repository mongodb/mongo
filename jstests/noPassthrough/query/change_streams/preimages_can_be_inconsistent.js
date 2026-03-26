/**
 * Test that consistency checks for change stream preimages work as expected. Consistency is defined by performing
 * these steps:
 *   * Fix a nsUUID to scan the change streams preimage collection
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

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {systemUsesReplicatedTruncates} from "jstests/libs/query/replicated_truncates_utils.js";
import {describe, it, beforeEach, afterEach} from "jstests/libs/mochalite.js";

describe("change streams preimages consistency checks", function () {
    const kPreimagesCollName = "system.preimages";

    const getPreImage = (collectionIndex, ts) => {
        const farOffDate = ISODate("2100-01-01");
        const epochSeconds = farOffDate.valueOf() / 1000;
        // Return a document inserted with a date really far off into the future.
        return {
            _id: {
                nsUUID: UUID(`3b241101-e2bb-4255-8caf-4136c566a12${collectionIndex}`),
                ts: new Timestamp(epochSeconds, ts),
                applyOpsIndex: 0,
            },
            operationTime: farOffDate,
        };
    };

    // Verifies that the replica set can be shut down without running into consistency check errors.
    const assertShutdownSucceedsWithoutHashMismatch = () => {
        // Note that when we get here, the contents of the preimages collection differs between the
        // primary and the secondary.
        // Verify that even if the data isn't consistent the test passes as consistency is defined
        // as two nodes having entries equal or non-existent starting from the end.
        replSetTest.checkPreImageCollection();

        // The shutdown is different depending on whether the system uses replicated truncates.
        if (systemUsesReplicatedTruncates(replSetTest.getPrimary())) {
            // When using replicated truncates, the set then needs to be stopped without performing
            // the regular consistency check, because that would detect the actual differences
            // between the primary and secondary that the relaxed check ignores!
            replSetTest.stopSet(undefined, undefined, {skipCheckDBHashes: true});
        } else {
            // When not using replicated truncates, the preimages collection is allowed to have
            // differences between the primary and secondary. The consistency check on shutdown will
            // report these differences but succeed nonetheless.
            replSetTest.stopSet();
        }
        replSetTest = null;
    };

    // Verifies that the replica set produces consistency check errors on shut down.
    const assertShutdownFailsWithHashMismatch = () => {
        // The test expects a hash mismatch between the primary and the secondary node.
        try {
            replSetTest.stopSet();
            assert(false, "Expected an inconsistency error when stopping the replica set, but no error was thrown.");
        } catch (e) {
            // Verify that the inconsistency is the one we're looking for in preimages.
            assert.eq(e.message.includes("non-matching preimage entries"), true);
        }

        // Tear down the nodes now without checking for consistency.
        replSetTest.stopSet(undefined, undefined, {skipCheckDBHashes: true});
        replSetTest = null;
    };

    let replSetTest = null;

    beforeEach(() => {
        // Start replica set with change stream preimages removal job turned off on all nodes.
        replSetTest = new ReplSetTest({
            name: "replSet",
            nodes: 2,
            nodeOptions: {setParameter: {disableExpiredPreImagesRemover: true}},
        });
        replSetTest.startSet();
        replSetTest.initiate();
    });

    afterEach(() => {
        assert.eq(null, replSetTest, "Expected replSetTest to be shut down by sub-afterEach blocks");
    });

    it("checks that there is no hash mismatch for identical preimages", () => {
        const primary = replSetTest.getPrimary();
        const secondary = replSetTest.getSecondary();

        const coll = primary.getDB("config")[kPreimagesCollName];
        const secondaryColl = secondary.getDB("config")[kPreimagesCollName];

        // Insert documents into the preimages collection on the primary, without replicating
        // them.
        coll.insert(getPreImage(1, 0));
        coll.insert(getPreImage(1, 1));
        coll.insert(getPreImage(2, 1));
        coll.insert(getPreImage(3, 1));

        // Ensure only the primary has the preimages.
        assert.eq(coll.find({}).itcount(), 4);
        assert.eq(secondaryColl.find({}).itcount(), 0);

        // Now insert the same preimages into the old secondary.
        replSetTest.stepUp(secondary);

        const newPrimary = replSetTest.getPrimary();
        const newColl = newPrimary.getDB("config")[kPreimagesCollName];
        newColl.insert(getPreImage(1, 0));
        newColl.insert(getPreImage(1, 1));
        newColl.insert(getPreImage(2, 1));
        newColl.insert(getPreImage(3, 1));

        assertShutdownSucceedsWithoutHashMismatch();
    });

    it("checks that there is no hash mismatch if one node has no preimages", () => {
        const primary = replSetTest.getPrimary();
        const secondary = replSetTest.getSecondary();

        const coll = primary.getDB("config")[kPreimagesCollName];
        const secondaryColl = secondary.getDB("config")[kPreimagesCollName];

        // Insert documents into the preimages collection on the primary, without replicating
        // them.
        coll.insert(getPreImage(1, 0));
        coll.insert(getPreImage(1, 1));
        coll.insert(getPreImage(2, 1));
        coll.insert(getPreImage(3, 1));

        // Ensure only the primary has the preimages.
        assert.eq(coll.find({}).itcount(), 4);
        assert.eq(secondaryColl.find({}).itcount(), 0);

        // Step up the old secondary, but do not insert any preimages there.
        replSetTest.stepUp(secondary);

        assertShutdownSucceedsWithoutHashMismatch();
    });

    it("checks that there is no hash mismatch if one node has a subset of preimages tails", () => {
        const primary = replSetTest.getPrimary();
        const secondary = replSetTest.getSecondary();

        const coll = primary.getDB("config")[kPreimagesCollName];
        const secondaryColl = secondary.getDB("config")[kPreimagesCollName];

        // Insert documents into the preimages collection on the primary, without replicating
        // them.
        coll.insert(getPreImage(1, 0));
        coll.insert(getPreImage(1, 1));
        coll.insert(getPreImage(2, 1));
        coll.insert(getPreImage(3, 1));

        // Ensure only the primary has the preimages.
        assert.eq(coll.find({}).itcount(), 4);
        assert.eq(secondaryColl.find({}).itcount(), 0);

        // Now insert a subset of the preimages into the old secondary. For the collections for
        // which preimages are inserted (1, 2), the preimages are a tail subset of the preimages
        // on the primary.
        replSetTest.stepUp(secondary);

        const newPrimary = replSetTest.getPrimary();
        const newColl = newPrimary.getDB("config")[kPreimagesCollName];
        newColl.insert(getPreImage(1, 1));
        newColl.insert(getPreImage(2, 1));

        assertShutdownSucceedsWithoutHashMismatch();
    });

    it("checks that there is a hash mismatch in case a non-tail preimage is missing on one node", () => {
        const primary = replSetTest.getPrimary();
        const secondary = replSetTest.getSecondary();

        const coll = primary.getDB("config")[kPreimagesCollName];
        const secondaryColl = secondary.getDB("config")[kPreimagesCollName];

        // Insert a few documents to the preimage collection. Ensure it is not replicated to secondaries.
        coll.insert(getPreImage(1, 0));
        coll.insert(getPreImage(1, 1));
        coll.insert(getPreImage(1, 2));
        coll.insert(getPreImage(1, 3));
        coll.insert(getPreImage(1, 4));
        coll.insert(getPreImage(1, 5));
        coll.insert(getPreImage(1, 6));
        coll.insert(getPreImage(1, 7));
        assert.eq(coll.find({}).itcount(), 8);
        assert.eq(secondaryColl.find({}).itcount(), 0);

        // Now insert another set of documents to the secondary, this will cause an
        // inconsistency error when stopping replica set.
        replSetTest.stepUp(secondary);

        const newPrimary = replSetTest.getPrimary();

        const newColl = newPrimary.getDB("config")[kPreimagesCollName];
        newColl.insert(getPreImage(1, 0));
        newColl.insert(getPreImage(1, 1));
        newColl.insert(getPreImage(1, 2));
        newColl.insert(getPreImage(1, 3));
        newColl.insert(getPreImage(1, 5));
        newColl.insert(getPreImage(1, 6));
        newColl.insert(getPreImage(1, 7));

        assertShutdownFailsWithHashMismatch();
    });

    it("checks that there is a hash mismatch in case the tail is different", () => {
        const primary = replSetTest.getPrimary();
        const secondary = replSetTest.getSecondary();

        const coll = primary.getDB("config")[kPreimagesCollName];
        const secondaryColl = secondary.getDB("config")[kPreimagesCollName];

        // Insert a few documents to the preimage collection. Ensure it is not replicated to secondaries.
        coll.insert(getPreImage(1, 0));
        coll.insert(getPreImage(1, 1));
        coll.insert(getPreImage(1, 2));
        assert.eq(coll.find({}).itcount(), 3);
        assert.eq(secondaryColl.find({}).itcount(), 0);

        // Now insert another set of documents to the secondary, this will cause an
        // inconsistency error when stopping replica set.
        replSetTest.stepUp(secondary);

        const newPrimary = replSetTest.getPrimary();

        const newColl = newPrimary.getDB("config")[kPreimagesCollName];
        newColl.insert(getPreImage(1, 42));

        assertShutdownFailsWithHashMismatch();
    });
});
