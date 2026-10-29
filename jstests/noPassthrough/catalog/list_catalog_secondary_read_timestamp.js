/**
 * Tests that collectionless $listCatalog on secondaries with rc:local reads at lastApplied rather
 * than kNoTimestamp.
 *
 * CommonMongodProcessInterface::listCatalog() has two code paths:
 *   1. No system.views on any database: early return with just a GlobalLock(MODE_IS).
 *   2. system.views exists: goes through acquireCollectionsMaybeLockFree which properly adjusts the
 *      read source.
 *
 * @tags: [
 *     requires_replication,
 *     assumes_read_concern_unchanged,
 * ]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";

const kFailpoint = "pauseBatchApplicationBeforeCompletion";

/**
 * Verifies that collectionless $listCatalog with rc:local on a secondary does not expose
 * collections from oplog batches that have been written to storage but not yet committed
 * (i.e., lastApplied has not advanced).
 */
function testListCatalogDuringPausedBatch(rst, primary, secondary, dbName, existingColl) {
    const primaryDB = primary.getDB(dbName);
    const secondaryAdminDB = secondary.getDB("admin");

    // Pause the secondary's oplog applier after it writes a batch to storage but before it
    // advances lastApplied. This creates a window where data is on disk but not yet "committed"
    // from a replication consistency standpoint.
    const fp = configureFailPoint(secondary, kFailpoint);

    // Create a new collection on the primary. Its oplog entry will be picked up by the
    // secondary and applied to storage, then the applier will block at the failpoint.
    assert.commandWorked(primaryDB.createCollection("coll_invisible"));

    // Wait until the secondary actually hits the failpoint, confirming the batch has been
    // applied to storage but lastApplied has not advanced.
    fp.wait();

    try {
        // Run collectionless $listCatalog with rc:local on the secondary. Because the read
        // should happen at lastApplied, the collection from the paused batch must not appear.
        const entries = secondaryAdminDB
            .aggregate([{$listCatalog: {}}, {$match: {db: dbName}}], {
                readConcern: {level: "local"},
            })
            .toArray();
        const names = entries.map((e) => e.name);

        assert(
            names.includes(existingColl),
            `Expected '${existingColl}' to be visible: ${tojson(names)}. $listCatalog on secondary (db: ${dbName}): ${tojson(entries)}`,
        );
        assert(
            !names.includes("coll_invisible"),
            `'coll_invisible' should NOT be visible while batch is paused ` +
                `(read should be at lastApplied): ${tojson(names)}. $listCatalog on secondary (db: ${dbName}): ${tojson(entries)}`,
        );
    } finally {
        fp.off();
    }

    // Let the batch complete and replicate fully before the next test case.
    rst.awaitReplication();
}

describe("collectionless $listCatalog respects read timestamp on secondaries", function () {
    before(function () {
        this.rst = new ReplSetTest({
            nodes: [{}, {rsConfig: {priority: 0, votes: 0}}],
            nodeOptions: {
                setParameter: {minSnapshotHistoryWindowInSeconds: 3600},
            },
        });
        this.rst.startSet();
        this.rst.initiate();
        this.primary = this.rst.getPrimary();
        this.secondary = this.rst.getSecondary();
    });

    after(function () {
        this.rst.stopSet();
    });

    it("reads at lastApplied when no system.views exists", function () {
        const dbName = jsTestName() + "_no_views";
        const primaryDB = this.primary.getDB(dbName);
        const secondaryDB = this.secondary.getDB(dbName);

        primaryDB.dropDatabase();
        assert.commandWorked(primaryDB.createCollection("coll_existing"));
        this.rst.awaitReplication();

        assert.eq(
            0,
            secondaryDB.getCollection("system.views").find().itcount(),
            "Expected no system.views on secondary",
        );

        testListCatalogDuringPausedBatch(
            this.rst,
            this.primary,
            this.secondary,
            dbName,
            "coll_existing",
        );
    });

    it("reads at lastApplied when system.views exists", function () {
        const dbName = jsTestName() + "_with_views";
        const primaryDB = this.primary.getDB(dbName);
        const secondaryDB = this.secondary.getDB(dbName);

        primaryDB.dropDatabase();
        assert.commandWorked(primaryDB.createCollection("coll_existing"));
        assert.commandWorked(
            primaryDB.runCommand({create: "my_view", viewOn: "coll_existing", pipeline: []}),
        );
        this.rst.awaitReplication();

        assert.gt(
            secondaryDB.getCollection("system.views").find().itcount(),
            0,
            "Expected system.views to exist on secondary",
        );

        testListCatalogDuringPausedBatch(
            this.rst,
            this.primary,
            this.secondary,
            dbName,
            "coll_existing",
        );
    });
});
