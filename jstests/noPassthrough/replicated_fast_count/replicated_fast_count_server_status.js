/**
 * This test checks that ReplicatedFastCountManager server status metrics are reported correctly.
 *
 * All metrics are reported via the OTel server status integration.
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
const kServerStatusAssertTimeoutMs = 10_000; // 10 sec

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
 * Returns the replicatedFastCount server status metrics subtree.
 */
function getMetrics(db) {
    return db.serverStatus().metrics?.replicatedFastCount ?? {};
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
        {
            // First flush with 500ms delay.
            const forceFlush = makeForceFlush(this.db, 500);
            forceFlush();

            assert.soon(
                () => {
                    const metrics = getMetrics(this.db);
                    return (
                        metrics.flush.successCount >= 1 &&
                        metrics.flushTime.total >= 500 &&
                        metrics.writeTime.total >= 0
                    );
                },
                () => `Expected flush time metrics after 500ms delay, got ${tojson(getMetrics(this.db))}`,
                kServerStatusAssertTimeoutMs,
            );
        }
        {
            // Second flush with 100ms delay.
            const forceFlush = makeForceFlush(this.db, 100);
            forceFlush();

            assert.soon(
                () => {
                    const metrics = getMetrics(this.db);
                    return metrics.flush.successCount >= 2 && metrics.flushTime.total >= 600;
                },
                () => `Expected flush time metrics after second flush, got ${tojson(getMetrics(this.db))}`,
                kServerStatusAssertTimeoutMs,
            );
        }
    });

    it("flush failure", function () {
        const failpoint = configureFailPoint(this.db, "failDuringFlush");
        assert.commandWorked(this.db.adminCommand({fsync: 1}));
        failpoint.wait();

        assert.soon(
            () => getMetrics(this.db).flush.failureCount == 1,
            () => `Expected flushFailureCount to be incremented, got ${tojson(getMetrics(this.db))}}`,
            kServerStatusAssertTimeoutMs,
        );
    });

    it("collection write and flush size", function () {
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
                    // TODO SERVER-122992: Enforce correct update / inserts.
                    // metrics.insertCount >= 1 &&
                    // metrics.updateCount >= 1 &&
                    metrics.flushedDocs.total >= 6
                );
            },
            () => `Expected write and flushed docs metrics, got ${tojson(getMetrics(this.db))}`,
            kServerStatusAssertTimeoutMs,
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
        // isRunning is not yet set (startup() has not been called), so the OTel gauge has not
        // been written and the key may be absent. Either absent or 0 is correct here.
        const metrics = getMetrics(this.db);
        assert.neq(metrics.isRunning, 1, metrics);
    });

    it("after step up", function () {
        this.rst.initiate();
        const metrics = getMetrics(this.db);
        assert.eq(metrics.isRunning, 1, metrics);
    });

    it("after step down", function () {
        this.rst.initiate();

        // Step down the primary to trigger ReplicatedFastCountManager shutdown.
        this.db.adminCommand({replSetStepDown: 60, force: true});

        assert.soon(
            () => getMetrics(this.db).isRunning != 1,
            () => `Expected isRunning to not be 1 after stepdown, got ${tojson(getMetrics(this.db))}`,
            kServerStatusAssertTimeoutMs,
        );
    });

    afterEach(function () {
        this.rst.stopSet();
    });
});
