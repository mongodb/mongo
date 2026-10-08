/**
 * Tests that serverStatus contains an indexBuilds section with lastCommittedMillis.
 *
 * @tags: [
 *   # Primary-driven index builds aren't resumable.
 *   primary_driven_index_builds_incompatible,
 *   requires_persistence,
 *   requires_replication,
 * ]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {after, afterEach, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {
    IndexBuildTest,
    ResumableIndexBuildTest,
} from "jstests/noPassthrough/libs/index_builds/index_build.js";
import {extractUUIDFromObject} from "jstests/libs/uuid_util.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";

const collName = "t";
const dbName = "test";

describe("index build throughput metrics", function () {
    const keyCount = 1000;
    const minBytesPerKey = 4; // The fields are 4 bytes, so the index keys generated must be >= 4 bytes each.

    before(() => {
        this.conn = MongoRunner.runMongod();
        this.db = this.conn.getDB(dbName);
    });

    beforeEach(() => {
        this.coll = this.db.getCollection(collName);

        assert.commandWorked(
            this.coll.insertMany(Array.from({length: keyCount}, (_, i) => ({a: `foo${i}`}))),
        );
    });

    const phases = ["collectionScan", "bulkLoad", "drainWrites"];
    const perPhaseMetrics = ["keysProcessed", "bytesProcessed", "phaseDurationMicros"];

    // Gets the difference in metric values between two indexBuilds serverStatus sections, returned as an object with entries of the form {metric: {phase: delta}} for each metric.
    const perPhaseDelta = (current, original) =>
        Object.fromEntries(
            perPhaseMetrics.map((metric) => [
                metric,
                Object.fromEntries(
                    phases.map((phase) => [
                        phase,
                        current[phase][metric] - original[phase][metric],
                    ]),
                ),
            ]),
        );

    // Builds an ascending index on field 'a', pausing the index build at  `failPointName`, and running `duringFn` during the pause.
    const buildIndexPausedAt = (failPointName, failPointData, duringFn) => {
        const fp = configureFailPoint(this.db, failPointName, failPointData);
        const awaitCreateIndex = IndexBuildTest.startIndexBuild(
            this.conn,
            this.coll.getFullName(),
            {a: 1},
        );
        try {
            fp.wait();
            duringFn();
        } finally {
            fp.off();
            awaitCreateIndex();
        }
    };

    it("per-phase keys and bytes processed", () => {
        const before = this.db.serverStatus().metrics.indexBuilds;

        buildIndexPausedAt(
            "hangIndexBuildDuringBulkLoadPhase",
            {iteration: 0, indexNames: ["a_1"]},
            () => {
                const during = this.db.serverStatus().metrics.indexBuilds;
                const delta = perPhaseDelta(during, before);
                assert.eq(delta.keysProcessed, {
                    collectionScan: keyCount,
                    bulkLoad: 0,
                    drainWrites: 0,
                });
                assert.gte(delta.bytesProcessed.collectionScan, keyCount * minBytesPerKey, {delta});
                assert.eq(delta.bytesProcessed.bulkLoad, 0, {delta});
                assert.eq(delta.bytesProcessed.drainWrites, 0, {delta});
            },
        );

        const after = this.db.serverStatus().metrics.indexBuilds;
        const delta = perPhaseDelta(after, before);
        // Assert that we scanned and loaded the expected number of keys/bytes, and that we did not drain any side writes.
        assert.eq(delta.keysProcessed, {
            collectionScan: keyCount,
            bulkLoad: keyCount,
            drainWrites: 0,
        });
        assert.gte(delta.bytesProcessed.bulkLoad, keyCount * minBytesPerKey, {delta});
        assert.eq(delta.bytesProcessed.drainWrites, 0, {delta});
    });

    it("per-phase durations", () => {
        const before = this.db.serverStatus().metrics.indexBuilds;

        let during;
        buildIndexPausedAt(
            "hangIndexBuildDuringBulkLoadPhase",
            {iteration: 0, indexNames: ["a_1"]},
            () => {
                during = this.db.serverStatus().metrics.indexBuilds;
                assert.gt(perPhaseDelta(during, before).phaseDurationMicros.collectionScan, 0);
            },
        );

        const after = this.db.serverStatus().metrics.indexBuilds;
        assert.gt(perPhaseDelta(after, during).phaseDurationMicros.bulkLoad, 0);
    });

    it("per-phase drain writes", () => {
        const sideWriteCount = 10;
        const before = this.db.serverStatus().metrics.indexBuilds;

        // Write to the collection we are building an index on, which should result in side writes being recorded for the drain phase.
        buildIndexPausedAt("hangAfterIndexBuildDumpsInsertsFromBulk", {}, () => {
            assert.commandWorked(
                this.coll.insertMany(Array.from({length: sideWriteCount}, (_, i) => ({a: i}))),
            );
        });

        const after = this.db.serverStatus().metrics.indexBuilds;
        const delta = perPhaseDelta(after, before);
        assert.eq(delta.keysProcessed, {
            collectionScan: keyCount,
            bulkLoad: keyCount,
            drainWrites: sideWriteCount,
        });
        assert.gte(delta.bytesProcessed.drainWrites, sideWriteCount * minBytesPerKey, {delta});
        assert.gt(delta.phaseDurationMicros.drainWrites, 0, {delta});
    });

    it("phase totals equal the sum of the per-phase metrics", () => {
        const before = this.db.serverStatus().metrics.indexBuilds;

        // Side writes during the pause make all three phases record work.
        buildIndexPausedAt("hangAfterIndexBuildDumpsInsertsFromBulk", {}, () => {
            assert.commandWorked(
                this.coll.insertMany(Array.from({length: 10}, (_, i) => ({a: i}))),
            );
        });

        const after = this.db.serverStatus().metrics.indexBuilds;
        const delta = perPhaseDelta(after, before);
        for (const metric of perPhaseMetrics) {
            const perPhaseSum = phases.reduce((sum, phase) => sum + delta[metric][phase], 0);
            assert.eq(after[metric] - before[metric], perPhaseSum, `${metric} total`, {delta});
        }
    });

    it("docs and bytes scanned", () => {
        const minBytesPerDoc = 4; // Each document holds at least the 4-byte field value.
        const before = this.db.serverStatus().metrics.indexBuilds;

        assert.commandWorked(this.coll.createIndex({a: 1}));

        const after = this.db.serverStatus().metrics.indexBuilds;
        assert.eq(after.docsScanned - before.docsScanned, keyCount, {before, after});
        assert.gte(after.bytesScanned - before.bytesScanned, keyCount * minBytesPerDoc, {
            before,
            after,
        });
    });

    afterEach(() => {
        this.coll.drop();
    });

    after(() => {
        MongoRunner.stopMongod(this.conn);
    });
});

describe("index build failures", function () {
    before(() => {
        this.conn = MongoRunner.runMongod();
        this.db = this.conn.getDB(dbName);
    });

    beforeEach(() => {
        this.coll = this.db.getCollection(collName);
        assert.commandWorked(this.coll.insertMany(Array.from({length: 10}, () => ({a: "foo"}))));
    });

    it("failedDueToDuplicateKeyError", () => {
        // Check that creating an index over a collection with duplicate keys only increments
        // failedDueToDuplicateKeyError.
        assert.eq(0, this.db.serverStatus().indexBuilds.failedDueToDuplicateKeyError);
        assert.eq(0, this.db.serverStatus().metrics.operation.insertFailedDueToDuplicateKeyError);

        assert.commandFailedWithCode(
            this.coll.createIndex({a: 1}, {unique: 1}),
            ErrorCodes.DuplicateKey,
        );

        IndexBuildTest.assertIndexes(this.coll, 1, ["_id_"]);
        assert.eq(1, this.db.serverStatus().indexBuilds.failedDueToDuplicateKeyError);
        assert.eq(0, this.db.serverStatus().metrics.operation.insertFailedDueToDuplicateKeyError);

        this.coll.drop();

        // Check that inserting a duplicate key into a unique index only increments
        // insertFailedDueToDuplicateKeyError.
        assert.commandWorked(this.coll.createIndex({a: 1}, {unique: 1}));
        assert.commandWorked(this.coll.insert({a: "foo"}));
        assert.commandFailedWithCode(this.coll.insert({a: "foo"}), ErrorCodes.DuplicateKey);

        assert.eq(1, this.db.serverStatus().indexBuilds.failedDueToDuplicateKeyError);
        assert.eq(1, this.db.serverStatus().metrics.operation.insertFailedDueToDuplicateKeyError);
    });

    it("failedDueToManualCancellation", () => {
        assert.eq(0, this.db.serverStatus().indexBuilds.failedDueToManualCancellation);

        const fp = configureFailPoint(this.db, "hangAfterInitializingIndexBuild");
        const awaitCreateIndex = IndexBuildTest.startIndexBuild(
            this.conn,
            this.coll.getFullName(),
            {a: 1},
        );

        fp.wait();
        assert.commandWorked(this.coll.dropIndex({a: 1}));
        awaitCreateIndex({checkExitStatus: false});

        // Wait for index build to be fully aborted before reading metrics.
        IndexBuildTest.waitForIndexBuildToStop(this.db, this.coll.getName(), "a_1");

        IndexBuildTest.assertIndexes(this.coll, 1, ["_id_"]);
        assert.eq(1, this.db.serverStatus().indexBuilds.failedDueToManualCancellation);
    });

    afterEach(() => {
        this.coll.drop();
    });

    after(() => {
        MongoRunner.stopMongod(this.conn);
    });
});

describe("lastCommittedMillis", function () {
    before(() => {
        this.rst = new ReplSetTest({
            nodes: 1,
        });
    });

    beforeEach(() => {
        this.rst.startSet();
        this.rst.initiate();

        this.primary = this.rst.getPrimary();
        this.db = this.primary.getDB(dbName);
        this.coll = this.db.getCollection(collName);

        assert.commandWorked(this.coll.insertMany(Array.from({length: 10}, () => ({a: "foo"}))));
    });

    it("during steady state", () => {
        assert.eq(0, this.db.serverStatus().metrics.indexBuilds.phases.lastCommittedMillis);

        // Hang index build before completion to extend duration.
        const fp = configureFailPoint(this.primary, "hangIndexBuildBeforeCommit");
        const awaitCreateIndex = IndexBuildTest.startIndexBuild(
            this.primary,
            this.coll.getFullName(),
            {a: 1},
        );

        // Initiate the failpoint and then sleep for 1000ms to ensure duration >= 1000ms.
        fp.wait();
        sleep(1000);
        fp.off();

        // Wait for the parallel shell to exit.
        awaitCreateIndex();

        assert.lt(1000, this.db.serverStatus().metrics.indexBuilds.phases.lastCommittedMillis);
    });

    it("on resume", () => {
        assert.eq(0, this.db.serverStatus().metrics.indexBuilds.phases.lastCommittedMillis);

        // Hang index build before completion to require resume.
        const fp = configureFailPoint(this.primary, "hangIndexBuildBeforeCommit");
        const awaitCreateIndex = IndexBuildTest.startIndexBuild(
            this.primary,
            this.coll.getFullName(),
            {a: 1},
            {} /* options */,
            [ErrorCodes.InterruptedDueToReplStateChange],
        );

        // Ensure index build is in-progress before restarting, else it is not resumed.
        fp.wait();

        const buildUUID = extractUUIDFromObject(
            IndexBuildTest.assertIndexes(this.coll, 2, ["_id_"], ["a_1"], {
                includeBuildUUIDs: true,
            })["a_1"].buildUUID,
        );

        // Restart to trigger resume.
        this.rst.restart(0);

        // Wait for the parallel shell to exit.
        awaitCreateIndex();

        // Connect to new primary.
        this.primary = this.rst.getPrimary();
        this.db = this.primary.getDB(dbName);
        this.coll = this.db.getCollection(collName);

        // Ensure index build is completed before reading metrics.
        ResumableIndexBuildTest.assertCompleted(this.primary, this.coll, [buildUUID], ["a_1"]);

        assert.lt(0, this.db.serverStatus().metrics.indexBuilds.phases.lastCommittedMillis);
    });

    it("on restart", function () {
        assert.eq(0, this.db.serverStatus().metrics.indexBuilds.phases.lastCommittedMillis);

        // Hang index build before completion to require restart.
        const fp = configureFailPoint(this.primary, "hangIndexBuildBeforeCommit");
        const awaitCreateIndex = IndexBuildTest.startIndexBuild(
            this.primary,
            this.coll.getFullName(),
            {a: 1},
            {} /* options */,
            [ErrorCodes.InterruptedDueToReplStateChange],
        );

        // Ensure index build is in-progress before stopping and starting, else it is not restarted.
        fp.wait();

        const buildUUID = extractUUIDFromObject(
            IndexBuildTest.assertIndexes(this.coll, 2, ["_id_"], ["a_1"], {
                includeBuildUUIDs: true,
            })["a_1"].buildUUID,
        );

        // Kill process to require index build to start over. The `forRestart` flag is required to
        // preserve state between stop and start.
        this.rst.stop(
            0,
            9 /* signal */,
            {allowedExitCode: MongoRunner.EXIT_SIGKILL},
            {forRestart: true},
        );

        // Wait for the parallel shell to exit.
        awaitCreateIndex({checkExitStatus: false});

        this.rst.start(0, {} /* options */, true /* restart */);

        // Connect to new primary.
        this.primary = this.rst.getPrimary();
        this.db = this.primary.getDB(dbName);
        this.coll = this.db.getCollection(collName);

        // Ensure index build is completed before reading metrics.
        ResumableIndexBuildTest.assertCompleted(this.primary, this.coll, [buildUUID], ["a_1"]);

        assert.lt(0, this.db.serverStatus().metrics.indexBuilds.phases.lastCommittedMillis);
    });

    afterEach(() => {
        this.rst.stopSet();
    });
});
