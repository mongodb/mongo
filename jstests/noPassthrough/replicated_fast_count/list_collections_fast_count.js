/**
 * Verifies that listCollections with rawData:true emits fastCount fields when
 * featureFlagReplicatedFastCount and featureFlagContainerWrites are manually enabled, and that
 * timestampStoreTs appears exactly once across all collection entries.
 *
 * @tags: [
 *   requires_replication,
 *   requires_persistence,
 *   requires_fsync,
 *   requires_timeseries,
 * ]
 */
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

/**
 * Triggers a flush of the replicated fast count manager so size/count metadata is persisted and
 * visible to listCollections rawData.
 *
 * The fast count deltas are produced by an async oplog tailer thread, not synchronously by the
 * writes themselves. The first fsync flushes whatever the tailer has already scanned into the
 * buffer; because the tailer may not yet have caught up to the writes that just returned, a second
 * fsync is issued so that anything scanned in the interim is also flushed. Two fsyncs are enough to
 * guarantee the tailer-committed writes are persisted for this quiesced single-node test.
 */
function flushFastCount(db) {
    assert.commandWorked(db.adminCommand({fsync: 1}));
    assert.commandWorked(db.adminCommand({fsync: 1}));
}

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

        // TODO SERVER-117454: Remove this explicit check and use the featureFlagReplicatedFastCount
        // tag once the flag is enabled in all feature flag variants.
        if (!FeatureFlagUtil.isPresentAndEnabled(this.db, "ReplicatedFastCount")) {
            jsTest.log.info("Skipping test because featureFlagReplicatedFastCount is disabled");
            this.rst.stopSet();
            quit();
        }
        // TODO SERVER-108818: Remove this explicit check and use the featureFlagContainerWrites tag
        // once the flag is enabled in all feature flag variants.
        if (!FeatureFlagUtil.isPresentAndEnabled(this.db, "ContainerWrites")) {
            jsTest.log.info("Skipping test because featureFlagContainerWrites is disabled");
            this.rst.stopSet();
            quit();
        }

        // Create several collections and insert data so the fast count manager has entries to
        // persist for each one.
        for (const name of ["collA", "collB", "collC"]) {
            assert.commandWorked(this.db.createCollection(name));
            assert.commandWorked(this.db[name].insertMany([{x: 1}, {x: 2}]));
        }

        // Flush so the fast count manager persists size/count metadata and advances the timestamp
        // store. Both are required for fastCount fields to appear in listCollections.
        flushFastCount(this.db);
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
        assert.eq(1, withTs.length, "expected timestampStoreTs on exactly one collection entry", {
            entries,
        });
    });

    it("does not emit fastCount when rawData is not set", function () {
        const res = assert.commandWorked(this.db.runCommand({listCollections: 1}));
        const entries = res.cursor.firstBatch;

        for (const entry of entries) {
            assert(!entry.info?.fastCount, "fastCount should not appear without rawData", {entry});
        }
    });

    it("emits fastCount for timeseries collections in rawData mode", function () {
        const timeFieldName = "time";
        const tsCollName = "tsColl";
        assert.commandWorked(
            this.db.createCollection(tsCollName, {timeseries: {timeField: timeFieldName}}),
        );
        assert.commandWorked(this.db[tsCollName].insert({[timeFieldName]: new Date()}));

        // Flush so the fast count manager persists metadata for the timeseries collection. The
        // 'info.fastCount' field is only emitted once the replicated fast count store has persisted
        // content, which normally happens via a background flush.
        flushFastCount(this.db);

        const res = assert.commandWorked(this.db.runCommand({listCollections: 1, rawData: true}));
        const entries = res.cursor.firstBatch;

        // Viewless timeseries collections do not have a system.buckets namespace. In rawData mode,
        // listCollections exposes the physical collection under the original collection name.
        const entry = entries.find((e) => e.name === tsCollName);
        assert(entry, "timeseries collection missing from listCollections output", {
            tsCollName,
            entries,
        });

        const fastCount = entry.info?.fastCount;
        assert(fastCount, "fastCount missing from timeseries collection entry", {entry});
        assert(fastCount.hasOwnProperty("size"), "fastCount missing size", {entry});
        assert(fastCount.hasOwnProperty("count"), "fastCount missing count", {entry});
        assert(fastCount.hasOwnProperty("validAsOf"), "fastCount missing validAsOf", {entry});
    });
});
