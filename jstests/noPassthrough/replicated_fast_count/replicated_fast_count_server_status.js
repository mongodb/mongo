/**
 * This test checks that ReplicatedFastCountManager server status metrics are reported correctly.
 *
 * @tags: [
 *   featureFlagReplicatedFastCount,
 *   requires_replication,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {afterEach, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

// Maximum amount of time that can elapse in an assert.soon() in this test before timeout. The
// default value is 10 minutes, but server status updates should never take that long.
const serverStatusAssertTimeoutMs = 10_000; // 10 sec

/**
 * Returns a function that triggers a flush of ReplicatedFastCountManager and waits for it to
 * complete. Can be called an arbitrary number of times within a single test.
 *
 * If a delay is provided, the thread that hits the failpoint sleeps for the specified number of
 * milliseconds. This is useful for artificially increasing the duration of a flush.
 */
function makeForceFlush(db, delay = 0) {
    const failpoint = configureFailPoint(db, "sleepAfterFlush", delay ? {sleepMs: delay} : {});
    let timesEntered = 0;
    return () => {
        assert.commandWorked(db.adminCommand({fsync: 1}));
        failpoint.wait({timesEntered: ++timesEntered});
    };
}

/**
 * Asserts the the provided array of field names exist in the replicated fast count server status
 * section.
 *
 * This is useful for detecting if a field name does not exist before asserting its value in
 * assert.soon() since assert.soon() does not print the difference between expected and actual
 * values.
 */
function checkMetricsExist(db, fieldNames) {
    const metrics = db.serverStatus().replicatedFastCount;
    for (const fieldName of fieldNames) {
        assert.neq(metrics[fieldName], undefined, metrics);
    }
}

/**
 * Returns the replicated fast count collection server status metrics.
 */
function getMetrics(db) {
    return db.serverStatus().replicatedFastCount;
}

describe("fast count server status metric", function () {
    beforeEach(function () {
        this.rst = new ReplSetTest({nodes: 1});
        this.rst.startSet();
        this.rst.initiate();
        const primary = this.rst.getPrimary();
        this.db = primary.getDB("test");
    });

    it("flush time", function () {
        checkMetricsExist(this.db, [
            "flushSuccessCount",
            "flushTimeMsMin",
            "flushTimeMsMax",
            "flushTimeMsTotal",
            "writeTimeMsTotal",
        ]);

        {
            // First flush with 500ms delay.
            const forceFlush = makeForceFlush(this.db, 500);
            forceFlush();

            assert.soon(
                () => {
                    const metrics = getMetrics(this.db);
                    return (
                        metrics.flushSuccessCount >= 1 &&
                        metrics.flushTimeMsMin >= 0 &&
                        metrics.flushTimeMsMax >= 500 &&
                        metrics.flushTimeMsTotal >= 500 &&
                        metrics.writeTimeMsTotal >= 0
                    );
                },
                () => `Expected flush time metrics after 500ms delay, got ${tojson(getMetrics(this.db))}`,
                serverStatusAssertTimeoutMs,
            );
        }
        {
            // Second flush with 100ms delay.
            const forceFlush = makeForceFlush(this.db, 100);
            forceFlush();

            assert.soon(
                () => {
                    const metrics = getMetrics(this.db);
                    return (
                        metrics.flushSuccessCount >= 2 &&
                        metrics.flushTimeMsMin < 500 &&
                        metrics.flushTimeMsMax >= 500 &&
                        metrics.flushTimeMsTotal >= 600
                    );
                },
                () => `Expected flush time metrics after second flush, got ${tojson(getMetrics(this.db))}`,
                serverStatusAssertTimeoutMs,
            );
        }
    });

    it("flush failure", function () {
        checkMetricsExist(this.db, ["flushFailureCount"]);

        const failpoint = configureFailPoint(this.db, "failDuringFlush");
        assert.commandWorked(this.db.adminCommand({fsync: 1}));
        failpoint.wait();

        assert.soon(
            () => {
                const metrics = getMetrics(this.db);
                return metrics.flushFailureCount == 1;
            },
            () => `Expected flushFailureCount to be incremented, got ${tojson(getMetrics(this.db))}}`,
            serverStatusAssertTimeoutMs,
        );
    });

    it("empty update", function () {
        const metrics = getMetrics(this.db);
        assert.eq(metrics.emptyUpdateCount, 0, metrics);
    });

    it("collection write and flush size", function () {
        checkMetricsExist(this.db, [
            "insertCount",
            "updateCount",
            "flushedDocsMin",
            "flushedDocsMax",
            "flushedDocsAvg",
        ]);

        for (let i = 1; i <= 5; i++) {
            assert.commandWorked(this.db.createCollection("coll" + i));
        }
        assert.commandWorked(this.db.coll1.insert({x: 1}));

        // Flush so coll1's metadata is persisted as an insert. The second insert into coll1 below
        // will then be recorded as an update rather than being batched with the first.
        const forceFlush = makeForceFlush(this.db);
        forceFlush();

        // Insert into all 5 collections. coll1 already has persisted metadata so it triggers an
        // update; colls 2-5 are new inserts. This also creates a batch of size >= 5.
        for (let i = 1; i <= 5; i++) {
            assert.commandWorked(this.db["coll" + i].insert({y: 1}));
        }
        forceFlush();

        assert.soon(
            () => {
                const metrics = getMetrics(this.db);
                return (
                    metrics.insertCount >= 1 &&
                    metrics.updateCount >= 1 &&
                    metrics.flushedDocsMin >= 1 &&
                    metrics.flushedDocsMax >= 5 &&
                    metrics.flushedDocsAvg > 0
                );
            },
            () => `Expected write and flushed docs metrics, got ${tojson(getMetrics(this.db))}`,
            serverStatusAssertTimeoutMs,
        );
    });

    afterEach(function () {
        this.rst.stopSet();
    });
});

describe("is running", function () {
    beforeEach(function () {
        this.rst = new ReplSetTest({nodes: 1});
        this.rst.startSet();
        this.db = this.rst.nodes[0].getDB("test");
    });

    it("before step up", function () {
        const metrics = getMetrics(this.db);
        assert.eq(metrics.isRunning, false, metrics);
    });

    it("after step up", function () {
        this.rst.initiate();
        const metrics = getMetrics(this.db);
        assert.eq(metrics.isRunning, true, metrics);
    });

    it("after step down", function () {
        this.rst.initiate();

        // Step down the primary to trigger ReplicatedFastCountManager shutdown.
        this.db.adminCommand({replSetStepDown: 60, force: true});

        assert.soon(
            () => getMetrics(this.db).isRunning === false,
            () => `Expected isRunning to be false after stepdown, got ${tojson(getMetrics(this.db))}`,
            serverStatusAssertTimeoutMs,
        );
    });

    afterEach(function () {
        this.rst.stopSet();
    });
});
