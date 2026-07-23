/**
 * Behavioral matrix for the query-memory load-shedding subsystem end-to-end against a real mongod.
 *
 * Load shedding is probabilistic and driven by process resident memory (RSS). A background monitor
 * samples RSS; at query yield/interrupt checkpoints a memory-tracked read operation is shed (killed
 * with QueryMemoryLimitExceeded) with a probability that scales with how far RSS is above a
 * low-water mark toward a high-water mark and with the operation's own tracked memory. There is no
 * spilling.
 * The dials:
 *   queryMemoryLoadSheddingLowMarkPercent  -1 disables; 0..100 is the RSS low-water mark.
 *   queryMemoryLoadSheddingHighMarkPercent RSS at/above which the pressure factor is 1.
 *   queryMemoryLoadSheddingSizeReferenceBytes operation size that maps to a size factor of 1.
 *
 * These tests drive the RSS signal deterministically through the test-only
 * 'queryMemoryPressureOverride' failpoint (data {usagePercent: N}) and force the probabilistic roll
 * with the test-only 'queryMemoryLoadSheddingAlwaysShed' failpoint, so an eligible operation (over the
 * low mark, not exempt, with positive tracked memory) is shed deterministically. The per-stage group
 * limit is kept high so the stage never self-fails before a checkpoint.
 *
 * The matrix runs under both query engines: both report tracked memory through the same
 * OperationMemoryUsageTracker path, so shedding is engine-independent -- running both exercises the
 * SBE tracker factories (e.g. SBE $group's hash_agg) that classic-only coverage misses.
 *
 * @tags: [
 *   requires_fcv_82,
 *   assumes_against_mongod_not_mongos,
 * ]
 */
import {assertEngine} from "jstests/libs/query/analyze_plan.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";

const kHuge = 512 * 1024 * 1024; // per-stage limit high enough that the group never self-fails

// The blocking $group used throughout: unique keys each pushing a 512-byte value (~10 MB tracked).
const kGroupPipeline = [{$group: {_id: "$g", vals: {$push: "$val"}}}];

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

// Deterministically set the process-wide RSS signal to 'pct' percent of the memory limit.
function setSignal(db, pct) {
    assert.commandWorked(
        db.adminCommand({
            configureFailPoint: "queryMemoryPressureOverride",
            mode: "alwaysOn",
            data: {usagePercent: pct},
        }),
    );
}

function clearSignal(db) {
    assert.commandWorked(
        db.adminCommand({configureFailPoint: "queryMemoryPressureOverride", mode: "off"}),
    );
}

// Force the probabilistic roll so any eligible operation is shed.
function forceShed(db, on) {
    assert.commandWorked(
        db.adminCommand({
            configureFailPoint: "queryMemoryLoadSheddingAlwaysShed",
            mode: on ? "alwaysOn" : "off",
        }),
    );
}

// Shedding is evaluated only at the operation's interrupt/yield checkpoints, so a single fast query
// is not guaranteed to be shed even with 'forceShed' on -- it must reach a checkpoint while eligible.
// Retry until an eligible run is actually shed (a genuinely broken feature would never shed, so this
// times out rather than passing). 'runFn' should run the eligible operation.
function assertEventuallyShed(runFn, msg) {
    assert.soon(() => {
        try {
            runFn();
            return false; // completed without being shed; try again
        } catch (e) {
            if (e.code === ErrorCodes.QueryMemoryLimitExceeded) {
                return true;
            }
            throw e; // any other error is a real failure
        }
    }, msg);
}

const kEngines = [
    {name: "classic", framework: "forceClassicEngine"},
    {name: "sbe", framework: "trySbeEngine"},
];

for (const engine of kEngines) {
    describe(`query memory behavior (${engine.name} engine)`, function () {
        let conn;
        let db;
        let coll;

        function runGroup() {
            return coll.aggregate(kGroupPipeline, {allowDiskUse: false}).toArray();
        }

        before(function () {
            conn = MongoRunner.runMongod({
                setParameter: {
                    queryMemoryLoadSheddingLowMarkPercent: 50,
                    queryMemoryLoadSheddingHighMarkPercent: 100,
                    // Tiny size reference so the ~10 MB group is always at the full size factor.
                    queryMemoryLoadSheddingSizeReferenceBytes: 1024,
                    internalDocumentSourceGroupMaxMemoryBytes: kHuge,
                    internalQuerySlotBasedExecutionHashAggApproxMemoryUseInBytesBeforeSpill: kHuge,
                    internalQueryFrameworkControl: engine.framework,
                },
            });
            assert.neq(null, conn, "mongod failed to start");
            db = conn.getDB("test");
            coll = db[jsTestName()];
            const docs = [];
            for (let i = 0; i < 20000; i++) {
                docs.push({_id: i, g: i, val: "x".repeat(512)});
            }
            assert.commandWorked(coll.insertMany(docs));
            clearSignal(db);
            forceShed(db, false);
            // Guard against silent classic fallback so this suite does not give false coverage.
            assertEngine(kGroupPipeline, engine.name, coll);
        });

        after(function () {
            MongoRunner.stopMongod(conn);
        });

        it("1. disabled (low mark -1): no shedding, queryMemory metrics absent", function () {
            setLowMark(db, -1);
            setSignal(db, 95); // strong signal, but shedding is disabled
            forceShed(db, true);
            try {
                assert.eq(
                    undefined,
                    memStats(db).loadShedding,
                    "queryMemory load-shedding metrics should be absent when disabled",
                );
                assert.eq(20000, runGroup().length);
            } finally {
                forceShed(db, false);
                clearSignal(db);
                setLowMark(db, 50);
            }
        });

        it("2. signal below the low mark: operation not shed", function () {
            setLowMark(db, 50);
            setSignal(db, 10); // 10% << 50% low mark -> pressure 0
            forceShed(db, true); // even forced, zero pressure means zero probability
            try {
                assert.neq(
                    undefined,
                    memStats(db).loadShedding.memLimitBytes,
                    "queryMemory load-shedding metrics should be present when enabled",
                );
                assert.eq(20000, runGroup().length, "below-low-mark query should succeed");
            } finally {
                forceShed(db, false);
                clearSignal(db);
            }
        });

        it("3. signal above the low mark: eligible operation is shed", function () {
            setLowMark(db, 50);
            setSignal(db, 90); // 90% > 50% low mark -> positive pressure
            forceShed(db, true);
            try {
                const before = memStats(db).loadShedding.operationsShed;
                assertEventuallyShed(runGroup, "over-low-mark query should be shed");
                assert.gt(
                    memStats(db).loadShedding.operationsShed,
                    before,
                    "expected operationsShed++",
                );
            } finally {
                forceShed(db, false);
                clearSignal(db);
            }
        });

        it("4. runtime toggle: enabling/disabling the low mark flips shedding live", function () {
            setSignal(db, 90);
            forceShed(db, true);
            try {
                setLowMark(db, -1); // disabled -> not shed
                assert.eq(20000, runGroup().length);

                setLowMark(db, 50); // enabled, over the mark -> shed
                assertEventuallyShed(runGroup, "re-enabled over-mark query should be shed");

                setLowMark(db, -1); // disabled again -> not shed
                assert.eq(20000, runGroup().length);
            } finally {
                forceShed(db, false);
                clearSignal(db);
                setLowMark(db, 50);
            }
        });

        it("5. writes are exempt: a multi:true update over the mark is not shed", function () {
            setLowMark(db, 50);
            setSignal(db, 95); // same over-mark signal that sheds a read in case 3
            forceShed(db, true);
            try {
                // A multi:true update matches every document, accumulating the update-dedup RecordIdSet
                // through the same operation memory tracker. Writes are exempt: killing a multi write
                // mid-batch with a RetriableError could make a naive client retry re-apply
                // non-idempotent modifiers, so it must run to completion instead of being shed.
                const res = assert.commandWorked(
                    db.runCommand({
                        update: coll.getName(),
                        updates: [{q: {}, u: {$inc: {n: 1}}, multi: true}],
                    }),
                );
                assert.eq(20000, res.n, res);
                assert.eq(20000, res.nModified, res);
            } finally {
                forceShed(db, false);
                clearSignal(db);
                // Undo the mutation so the collection stays at its seeded state.
                assert.commandWorked(
                    db.runCommand({
                        update: coll.getName(),
                        updates: [{q: {}, u: {$unset: {n: ""}}, multi: true}],
                    }),
                );
            }
        });

        it("6. deletes are exempt: a multi:true delete over the mark is not shed", function () {
            setLowMark(db, 50);
            setSignal(db, 95); // over-mark signal that would shed an eligible read
            forceShed(db, true);
            try {
                // A multi:true delete runs inside the write exemption window, so it completes rather
                // than being shed after a partial, non-retryable delete.
                const res = assert.commandWorked(
                    db.runCommand({delete: coll.getName(), deletes: [{q: {}, limit: 0}]}),
                );
                assert.eq(20000, res.n, res);
            } finally {
                forceShed(db, false);
                clearSignal(db);
                // Re-seed so the collection stays at its seeded state for any later cases/reruns.
                const docs = [];
                for (let i = 0; i < 20000; i++) {
                    docs.push({_id: i, g: i, val: "x".repeat(512)});
                }
                assert.commandWorked(coll.insertMany(docs));
            }
        });

        it("7. releaseMemory spills a cursor instead of shedding and killing it", function () {
            setLowMark(db, 50);
            // Open a spillable $group cursor and keep it pinnable with a stashed executor.
            const openRes = assert.commandWorked(
                db.runCommand({
                    aggregate: coll.getName(),
                    pipeline: kGroupPipeline,
                    allowDiskUse: true,
                    cursor: {batchSize: 1},
                }),
            );
            const cursorId = openRes.cursor.id;
            assert.neq(cursorId, 0, "expected an open cursor with more data to spill");
            try {
                setSignal(db, 95); // over-mark: without the exemption the spill would be shed
                forceShed(db, true);
                // releaseMemory spills inside the exemption window; without it the spill's interrupt
                // check would shed and destroy the very cursor being spilled.
                const rm = assert.commandWorked(db.runCommand({releaseMemory: [cursorId]}));
                assert.eq([cursorId], rm.cursorsReleased, rm);
                // The cursor survived and is still usable: a getMore succeeds rather than
                // CursorNotFound.
                assert.commandWorked(
                    db.runCommand({getMore: cursorId, collection: coll.getName(), batchSize: 1}),
                );
            } finally {
                forceShed(db, false);
                clearSignal(db);
                db.runCommand({killCursors: coll.getName(), cursors: [cursorId]});
            }
        });

        it("8. the blocking $group feeding a $merge is sheddable (before the write)", function () {
            // A $group|$merge pipeline: the $merge write is exempt (DocumentSourceWriteBlock), but
            // the upstream blocking $group is not -- it accumulates tracked memory before any write,
            // so an over-mark op is shed there. The shed happens before the $merge runs, so no
            // partial write reaches the destination.
            setLowMark(db, 50);
            setSignal(db, 90);
            forceShed(db, true);
            const dest = db[jsTestName() + "_merge_dest"];
            dest.drop();
            const mergePipeline = [
                {$group: {_id: "$g", vals: {$push: "$val"}}},
                {$merge: {into: dest.getName(), whenMatched: "replace", whenNotMatched: "insert"}},
            ];
            try {
                assertEventuallyShed(
                    () => coll.aggregate(mergePipeline, {allowDiskUse: false}).toArray(),
                    "the $group feeding $merge should be shed before the write",
                );
                assert.eq(
                    0,
                    dest.countDocuments({}),
                    "shed happened during $group, so nothing should have been written",
                );
            } finally {
                forceShed(db, false);
                clearSignal(db);
                dest.drop();
            }
        });
    });
}

// Note: the property "spilling does not disable shedding" -- i.e. a disk spill takes no write-intent
// lock, so it does not exempt an operation -- is covered deterministically by the
// ReadIntentLockedOperationIsStillShed unit test. It has no reliable jstest observable, because a
// spill drains an operation's tracked in-use memory, which is exactly what drives the shed decision:
// a heavily-spilled query legitimately stops being a memory-shed candidate.
