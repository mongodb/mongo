/**
 * Verifies that listCollections with rawData:true emits fastCount fields when
 * featureFlagReplicatedFastCount and featureFlagContainerWrites are manually enabled, and that
 * timestampStoreTs appears exactly once across all collection entries.
 *
 * @tags: [
 *   requires_replication,
 *   requires_persistence,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

describe("listCollections fastCount fields", function () {
    before(function () {
        this.rst = new ReplSetTest({
            nodes: 1,
            nodeOptions: {
                setParameter: {
                    featureFlagReplicatedFastCount: true,
                    featureFlagContainerWrites: true,
                },
            },
        });
        this.rst.startSet();
        this.rst.initiate();
        this.primary = this.rst.getPrimary();
        this.db = this.primary.getDB(jsTestName());

        // Create several collections and insert data so the fast count manager has entries to
        // persist for each one.
        for (const name of ["collA", "collB", "collC"]) {
            assert.commandWorked(this.db.createCollection(name));
            assert.commandWorked(this.db[name].insertMany([{x: 1}, {x: 2}]));
        }

        // Force a flush so the fast count manager persists size/count metadata and advances the
        // timestamp store. Both are required for fastCount fields to appear in listCollections.
        const fp = configureFailPoint(this.db, "sleepAfterFlush");
        assert.commandWorked(this.db.adminCommand({fsync: 1}));
        fp.wait();
        fp.off();
    });

    after(function () {
        this.rst.stopSet();
    });

    it("emits fastCount with size, count, and validAsOf on collections with persisted data", function () {
        const res = assert.commandWorked(this.db.runCommand({listCollections: 1, rawData: true}));
        const entries = res.cursor.firstBatch;

        for (const name of ["collA", "collB", "collC"]) {
            const entry = entries.find((e) => e.name === name);
            assert(entry, "collection missing from listCollections output", {name, entries});

            const fastCount = entry.info?.fastCount;
            assert(fastCount, "fastCount missing from collection entry", {entry});
            assert(fastCount.hasOwnProperty("size"), "fastCount missing size", {entry});
            assert(fastCount.hasOwnProperty("count"), "fastCount missing count", {entry});
            assert(fastCount.hasOwnProperty("validAsOf"), "fastCount missing validAsOf", {entry});
        }
    });

    it("emits timestampStoreTs exactly once across all collection entries", function () {
        const res = assert.commandWorked(this.db.runCommand({listCollections: 1, rawData: true}));
        const entries = res.cursor.firstBatch;

        const withTs = entries.filter((e) => e.info?.fastCount?.hasOwnProperty("timestampStoreTs"));
        assert.eq(1, withTs.length, "expected timestampStoreTs on exactly one collection entry", {entries});
    });

    it("does not emit fastCount when rawData is not set", function () {
        const res = assert.commandWorked(this.db.runCommand({listCollections: 1}));
        const entries = res.cursor.firstBatch;

        for (const entry of entries) {
            assert(!entry.info?.fastCount, "fastCount should not appear without rawData", {entry});
        }
    });
});
