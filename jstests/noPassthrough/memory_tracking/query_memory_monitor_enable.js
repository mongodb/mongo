/**
 * Verifies the RSS monitor is started lazily: it does not run while load shedding is disabled (the
 * default), and enabling the low-mark knob at runtime starts it (via the on-update hook), after
 * which the subsystem publishes real RSS samples.
 *
 * @tags: [
 *   requires_fcv_82,
 *   assumes_against_mongod_not_mongos,
 * ]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";

// serverStatus().queryMemory. Populated only while load shedding is enabled; an empty object
// otherwise.
function memStats(db) {
    return assert.commandWorked(db.adminCommand({serverStatus: 1})).queryMemory ?? {};
}

function setLowMark(db, pct) {
    assert.commandWorked(
        db.adminCommand({setParameter: 1, queryMemoryLoadSheddingLowMarkPercent: pct}),
    );
}

describe("query memory monitor lifecycle", function () {
    let conn;
    let db;

    before(function () {
        // Start with the feature disabled (-1, the default). Small monitor interval so that, once
        // enabled, the first RSS sample lands quickly.
        conn = MongoRunner.runMongod({
            setParameter: {
                queryMemoryLoadSheddingLowMarkPercent: -1,
                queryMemoryRssMonitorIntervalMillis: 50,
            },
        });
        assert.neq(null, conn, "mongod failed to start");
        db = conn.getDB("test");
    });

    after(function () {
        MongoRunner.stopMongod(conn);
    });

    it("starts the monitor on runtime enable and publishes an RSS sample", function () {
        // Disabled at startup: the queryMemory metrics are absent and no monitor runs.
        assert.eq(
            undefined,
            memStats(db).loadShedding,
            "queryMemory load-shedding metrics should be absent while disabled",
        );

        // Enable at runtime -> the on-update hook starts the monitor, which soon publishes a real
        // RSS sample. currentUsageBytes > 0 proves the monitor thread actually started now (it was
        // never started at process startup, since the feature was disabled then).
        setLowMark(db, 80);
        assert.soon(() => {
            return memStats(db).loadShedding?.currentUsageBytes > 0;
        }, "monitor should start and publish an RSS sample after enabling at runtime");

        // Re-disable -> the metrics disappear again.
        setLowMark(db, -1);
        assert.eq(
            undefined,
            memStats(db).loadShedding,
            "queryMemory load-shedding metrics should be absent after re-disabling",
        );

        // Re-enable -> the monitor restarts and publishes a fresh sample, so stop-then-start leaves a
        // working monitor rather than a wedged or never-restarted one.
        setLowMark(db, 80);
        assert.soon(() => {
            return memStats(db).loadShedding?.currentUsageBytes > 0;
        }, "monitor should restart and publish an RSS sample after re-enabling");
        setLowMark(db, -1);
    });

    it("rejects a low mark that is not below the high mark", function () {
        // High mark defaults to 85. A low mark at/above it would leave the feature enabled but
        // permanently inert (pressure never rises), so the cross-validator must reject it.
        try {
            assert.commandFailedWithCode(
                db.adminCommand({setParameter: 1, queryMemoryLoadSheddingLowMarkPercent: 90}),
                ErrorCodes.BadValue,
            );
            // Symmetrically, a high mark at/below an enabled low mark is rejected.
            setLowMark(db, 70);
            assert.commandFailedWithCode(
                db.adminCommand({setParameter: 1, queryMemoryLoadSheddingHighMarkPercent: 70}),
                ErrorCodes.BadValue,
            );
            // A well-ordered pair is accepted.
            assert.commandWorked(
                db.adminCommand({setParameter: 1, queryMemoryLoadSheddingHighMarkPercent: 90}),
            );
        } finally {
            assert.commandWorked(
                db.adminCommand({setParameter: 1, queryMemoryLoadSheddingHighMarkPercent: 85}),
            );
            setLowMark(db, -1);
        }
    });
});

describe("query memory monitor started at startup", function () {
    let conn;
    let db;

    before(function () {
        // Enable the low mark at process startup so startQueryMemoryRssMonitor() runs during mongod
        // startup rather than via the runtime on-update hook. Small interval for a quick first
        // sample.
        conn = MongoRunner.runMongod({
            setParameter: {
                queryMemoryLoadSheddingLowMarkPercent: 80,
                queryMemoryRssMonitorIntervalMillis: 50,
            },
        });
        assert.neq(null, conn, "mongod failed to start");
        db = conn.getDB("test");
    });

    after(function () {
        MongoRunner.stopMongod(conn);
    });

    it("publishes an RSS sample without any runtime toggle", function () {
        // A real sample proves the monitor started during startup; this fails if startup ran the
        // monitor before the PeriodicRunner existed (null runner -> silent no-op).
        assert.soon(
            () => memStats(db).loadShedding?.currentUsageBytes > 0,
            "monitor started at startup should publish an RSS sample",
        );
    });
});
