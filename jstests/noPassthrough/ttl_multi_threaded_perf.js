/**
 * Perf skeleton for SERVER-56195 — multi-threaded TTL monitor.
 *
 * Sweeps ttlMonitorDeleteWorkers ∈ {1, 2, 4, 8} across three workload
 * shapes (many-small-indexes, one-fat-index, WT-pressure-adversarial)
 * and reports docs-deleted/sec measured against the
 * ttl.deletedDocuments serverStatus counter.
 *
 * This is a SKELETON. The actual concurrency knob does not yet exist;
 * landing this test depends on the IDL + cpp changes proposed in
 * src/mongo/db/ttl/MULTI_THREADED_TTL_DESIGN.md.
 *
 * @tags: [
 *   requires_persistence,
 *   requires_replication,
 *   resource_intensive,
 *   does_not_support_stepdowns,
 * ]
 */

import {TTLUtil} from "jstests/libs/ttl/ttl_util.js";

const kWorkerSweep = [1, 2, 4, 8];
const kSteadyStateSecs = 60;
const kWarmupSecs = 15;

function startServer(workers) {
    return MongoRunner.runMongod({
        setParameter: {
            ttlMonitorEnabled: true,
            ttlMonitorSleepSecs: 1,
            ttlMonitorBatchDeletes: true,
            ttlMonitorSubPassTargetSecs: 5,
            ttlIndexDeleteTargetTimeMS: 1000,
            ttlIndexDeleteTargetDocs: 50000,
            // The knob under test. When unimplemented, mongod will reject
            // the parameter — the assert below makes that obvious.
            ttlMonitorDeleteWorkers: workers,
        },
    });
}

function readTTLCounters(conn) {
    const ss = conn.getDB("admin").serverStatus();
    return {
        deletedDocs: ss.metrics.ttl.deletedDocuments,
        passes: ss.metrics.ttl.passes,
        subPasses: ss.metrics.ttl.subPasses,
    };
}

function loadShapeA(conn) {
    // 64 collections, one TTL index each, 100k docs per collection.
    const db = conn.getDB("ttl_perf_A");
    for (let i = 0; i < 64; i++) {
        const coll = db.getCollection(`c${i}`);
        coll.createIndex({ts: 1}, {expireAfterSeconds: 30});
        const bulk = coll.initializeUnorderedBulkOp();
        const now = new Date();
        for (let j = 0; j < 100000; j++) {
            bulk.insert({ts: new Date(now.getTime() - 60000), j: j});
        }
        bulk.execute();
    }
}

function loadShapeB(conn) {
    // One collection, 10M docs, single fat TTL index.
    const coll = conn.getDB("ttl_perf_B").c;
    coll.createIndex({ts: 1}, {expireAfterSeconds: 60});
    const now = new Date();
    for (let chunk = 0; chunk < 100; chunk++) {
        const bulk = coll.initializeUnorderedBulkOp();
        for (let j = 0; j < 100000; j++) {
            bulk.insert({ts: new Date(now.getTime() - 120000), j: j});
        }
        bulk.execute();
    }
}

function loadShapeC(conn) {
    // Shape A baseline plus an adversarial concurrent inserter.
    loadShapeA(conn);
    // Concurrent bulk inserter — left as a Thread() spawn in the real
    // implementation. Skeleton stops short of spawning to keep the
    // dependency surface minimal until the underlying knob lands.
    // TODO(SERVER-56195): wire startParallelShell() inserter at 50 MB/s.
}

function measure(conn, label) {
    sleep(kWarmupSecs * 1000);
    const t0 = Date.now();
    const c0 = readTTLCounters(conn);
    sleep(kSteadyStateSecs * 1000);
    const c1 = readTTLCounters(conn);
    const elapsedSecs = (Date.now() - t0) / 1000;
    const docsPerSec = (c1.deletedDocs - c0.deletedDocs) / elapsedSecs;
    jsTestLog(`[ttl-perf] ${label}: ${docsPerSec.toFixed(1)} docs/s ` +
              `(passes=${c1.passes - c0.passes} subPasses=${c1.subPasses - c0.subPasses})`);
    return docsPerSec;
}

function sweepShape(shapeName, loader) {
    const results = {};
    for (const workers of kWorkerSweep) {
        const conn = startServer(workers);
        loader(conn);
        results[workers] = measure(conn, `${shapeName} workers=${workers}`);
        MongoRunner.stopMongod(conn);
    }
    return results;
}

// Entry point — guarded so the file can be sourced without firing the
// full sweep in CI smoke. The full sweep is gated on a sys-perf task.
if (TestData && TestData.ttlMultiThreadedFullSweep) {
    const shapeA = sweepShape("A_many_small", loadShapeA);
    const shapeB = sweepShape("B_one_fat", loadShapeB);
    const shapeC = sweepShape("C_wt_pressure", loadShapeC);

    // Pass criterion lives in design doc §6: A ≥ 1.8× at workers=4 vs 1.
    const aSpeedup = shapeA[4] / shapeA[1];
    const bRegression = shapeB[4] / shapeB[1];
    jsTestLog(`[ttl-perf] Shape A speedup @ workers=4: ${aSpeedup.toFixed(2)}x ` +
              `(target ≥ 1.8x)`);
    jsTestLog(`[ttl-perf] Shape B ratio @ workers=4: ${bRegression.toFixed(2)}x ` +
              `(target within ±10% of 1.0)`);
    assert.gte(aSpeedup, 1.8, "Shape A multi-thread speedup below target");
    assert.gte(bRegression, 0.9, "Shape B regressed beyond ±10% tolerance");
    assert.lte(bRegression, 1.1, "Shape B improved beyond expectation — investigate");
} else {
    jsTestLog("[ttl-perf] Skeleton sourced without TestData.ttlMultiThreadedFullSweep; " +
              "set the flag to run the worker sweep.");
}
