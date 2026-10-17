/**
 * End-to-end test for idle-cursor query-memory load shedding (SERVER-132002).
 *
 * A query that holds a large batch in memory and then sits idle between getMores should be eligible
 * for query memory load shedding. The client sees QueryMemoryLimitExceeded on its next getMore, and
 * the per-cursor log line explains what happened.
 *
 * Drive the RSS signal and the shed roll deterministically through the test-only failpoints, same
 * as in query_memory_behavior.js, which tests the active shedding path.
 *
 * @tags: [
 *   requires_fcv_91,
 *   assumes_against_mongod_not_mongos,
 * ]
 */
import {assertEngine} from "jstests/libs/query/analyze_plan.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {checkLog} from "src/mongo/shell/check_log.js";

const kHuge = 512 * 1024 * 1024; // per-stage limit high enough that the group never self-fails

// A $group that accumulates ~1 MB of tracked memory and produces more than one batch, so the cursor
// survives the first batch and goes idle.
const kGroupPipeline = [{$group: {_id: "$g", vals: {$push: "$val"}}}];
// 1 MB of data in 1 KB documents. The size is not critical: the idle-cursor sweep reads the
// cursor's memory tracker, whose total is always exact. Only CurOp's copy is updated in chunks of
// internalQueryMaxWriteToCurOpMemoryUsageBytes (1 MB by default), and only the queued-operation
// sweep reads that copy.
const kNumDocs = 1024;
const kDocSize = 1024;

// The idle-cursor sweep interval: shorter than the 1 s default, to speed up this test.
const kSweepMillis = 100;

// A tiny size reference, so the ~1 MB group is always at the full size factor.
const kSizeReferenceBytes = 128;

function memStats(db) {
    return assert.commandWorked(db.adminCommand({serverStatus: 1})).queryMemory ?? {};
}

function setLowMark(db, pct) {
    assert.commandWorked(
        db.adminCommand({setParameter: 1, queryMemoryLoadSheddingLowMarkPercent: pct}),
    );
}

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

function forceSheddingFailpoint(db, on) {
    assert.commandWorked(
        db.adminCommand({
            configureFailPoint: "queryMemoryLoadSheddingAlwaysShed",
            mode: on ? "alwaysOn" : "off",
        }),
    );
}

// Open a cursor with batchSize: 1, drain the first batch to unpin it, then return the cursor id
// without closing it — leaving the cursor idle on the server.
function openIdleCursor(coll) {
    const cursor = coll.aggregate(kGroupPipeline, {
        allowDiskUse: false,
        cursor: {batchSize: 1},
    });
    assert.gt(cursor.objsLeftInBatch(), 0);
    // Do NOT close: closing sends killCursors. Just read the id and stop iterating.
    return cursor.getId();
}

function getMore(db, coll, cursorId, batchSize) {
    return db.runCommand({
        getMore: cursorId,
        collection: coll.getName(),
        batchSize: batchSize,
    });
}

function killCursor(db, coll, cursorId) {
    assert.commandWorked(
        db.runCommand({
            killCursors: coll.getName(),
            cursors: [cursorId],
        }),
    );
}

// Run under both engines because they leave an idle cursor in different states: e.g., SBE stashes
// TransactionResources between getMores even outside a transaction.
const kEngines = [
    {name: "classic", framework: "forceClassicEngine"},
    {name: "sbe", framework: "trySbeEngine"},
];

for (const engine of kEngines) {
    describe(`idle-cursor query-memory load shedding (${engine.name} engine)`, function () {
        let conn;
        let db;
        let coll;

        before(function () {
            conn = MongoRunner.runMongod({
                setParameter: {
                    queryMemoryLoadSheddingLowMarkPercent: 50,
                    queryMemoryLoadSheddingHighMarkPercent: 100,
                    queryMemoryLoadSheddingSizeReferenceBytes: kSizeReferenceBytes,
                    queryMemoryIdleCursorSheddingIntervalMillis: kSweepMillis,
                    internalDocumentSourceGroupMaxMemoryBytes: kHuge,
                    internalQuerySlotBasedExecutionHashAggApproxMemoryUseInBytesBeforeSpill: kHuge,
                    internalQueryFrameworkControl: engine.framework,
                },
            });
            assert.neq(null, conn, "mongod failed to start");
            db = conn.getDB("test");
            coll = db[jsTestName()];
            const docs = [];
            for (let i = 0; i < kNumDocs; i++) {
                docs.push({_id: i, g: i, val: "x".repeat(kDocSize)});
            }
            assert.commandWorked(coll.insertMany(docs));
            clearSignal(db);
            forceSheddingFailpoint(db, false);
            assertEngine(kGroupPipeline, engine.name, coll);
        });

        after(function () {
            MongoRunner.stopMongod(conn);
        });

        it("1. idle cursor is shed under pressure; getMore sees QueryMemoryLimitExceeded; log explains it", function () {
            setLowMark(db, 50);
            // Open the cursor with pressure below the low mark so the opening aggregate is not
            // shed by the interrupt path before the cursor exists.
            setSignal(db, 10);
            forceSheddingFailpoint(db, false);

            try {
                const cursorId = openIdleCursor(coll);
                // Read before enabling pressure: the sweep can shed as soon as pressure is on. Use a
                // delta since idleCursorsShed is cumulative across tests sharing this mongod.
                const shedBefore = memStats(db).loadShedding.idleCursorsShed;

                // Now that the cursor is idle, enable pressure and force the shed.
                setSignal(db, 95);
                forceSheddingFailpoint(db, true);

                // Wait for the sweep to reap it. The shed is forced, so the only delay is the sweep
                // interval; allow ten seconds to cover scheduling jitter on slow hosts.
                assert.soon(
                    () => {
                        const shedStats = memStats(db).loadShedding;
                        return shedStats && shedStats.idleCursorsShed > shedBefore;
                    },
                    "idleCursorsShed did not increment",
                    10 * 1000,
                    kSweepMillis / 2,
                );

                // The first getMore fails with QueryMemoryLimitExceeded.
                const res = getMore(db, coll, cursorId, 1);
                assert.commandFailedWithCode(res, ErrorCodes.QueryMemoryLimitExceeded);

                // The reason is consumed: a second getMore degrades to CursorNotFound.
                const res2 = getMore(db, coll, cursorId, 1);
                assert.commandFailedWithCode(res2, ErrorCodes.CursorNotFound);

                // Reaped cursors are logged.
                // Match the id textually: a 64-bit cursor id loses precision if parsed from the JSON log.
                const idStr = String(cursorId).match(/\d+/)[0];
                assert.soon(() => checkLog.checkContainsOnce(conn, `"cursorId":${idStr},`));
            } finally {
                forceSheddingFailpoint(db, false);
                clearSignal(db);
            }
        });

        it("2. feature disabled: idle cursor survives even under pressure", function () {
            setLowMark(db, -1); // disable
            setSignal(db, 95);
            forceSheddingFailpoint(db, true);

            let cursorId;
            try {
                cursorId = openIdleCursor(coll);

                // No sweep runs while the feature is disabled, so after ten sweep intervals the
                // shed counters are still absent.
                sleep(10 * kSweepMillis);
                const shedStats = memStats(db).loadShedding;
                assert.eq(
                    undefined,
                    shedStats,
                    "load-shedding metrics should be absent when disabled",
                );

                // The cursor is still alive: a getMore succeeds.
                const res = assert.commandWorked(getMore(db, coll, cursorId, kNumDocs));
                assert.gt(res.cursor.nextBatch.length, 0);
            } finally {
                if (cursorId !== undefined) {
                    killCursor(db, coll, cursorId);
                }
                forceSheddingFailpoint(db, false);
                clearSignal(db);
                setLowMark(db, 50);
            }
        });

        it("3. pressure below the low mark: idle cursor is not shed", function () {
            setLowMark(db, 50);
            setSignal(db, 10); // below the low mark -> pressure 0
            forceSheddingFailpoint(db, true);

            let cursorId;
            try {
                cursorId = openIdleCursor(coll);

                // The sweep runs but the below-low-mark fast path skips every cursor.
                // Use a delta: idleCursorsShed is cumulative across tests sharing this mongod.
                const before = memStats(db).loadShedding.idleCursorsShed;
                sleep(10 * kSweepMillis);
                const after = memStats(db).loadShedding.idleCursorsShed;
                assert.eq(before, after, "no idle cursor should be shed below the low mark");

                // The cursor is still alive: a getMore succeeds.
                const res = assert.commandWorked(getMore(db, coll, cursorId, kNumDocs));
                assert.gt(res.cursor.nextBatch.length, 0);
            } finally {
                if (cursorId !== undefined) {
                    killCursor(db, coll, cursorId);
                }
                forceSheddingFailpoint(db, false);
                clearSignal(db);
            }
        });
    });
}

// The shed reason is returned only to the cursor's owner: same authenticated user and same session.
// Anyone else gets CursorNotFound and leaves the reason in place, so they can neither consume it nor
// learn that the cursor was shed.
describe("idle-cursor query-memory load shedding: shedding reason is owner-only", function () {
    let conn;
    let admin;

    before(function () {
        conn = MongoRunner.runMongod({
            auth: "",
            setParameter: {
                queryMemoryLoadSheddingLowMarkPercent: 50,
                queryMemoryLoadSheddingHighMarkPercent: 100,
                queryMemoryLoadSheddingSizeReferenceBytes: kSizeReferenceBytes,
                queryMemoryIdleCursorSheddingIntervalMillis: kSweepMillis,
                internalDocumentSourceGroupMaxMemoryBytes: kHuge,
                internalQuerySlotBasedExecutionHashAggApproxMemoryUseInBytesBeforeSpill: kHuge,
            },
        });
        assert.neq(null, conn, "mongod failed to start");
        admin = conn.getDB("admin");
        admin.createUser({user: "admin", pwd: "pwd", roles: ["root"]});
        assert(admin.auth("admin", "pwd"));
        for (const user of ["owner", "other"]) {
            admin.createUser({user: user, pwd: "pwd", roles: [{role: "readWrite", db: "test"}]});
        }
        const docs = [];
        for (let i = 0; i < kNumDocs; i++) {
            docs.push({_id: i, g: i, val: "x".repeat(kDocSize)});
        }
        assert.commandWorked(conn.getDB("test")[jsTestName()].insertMany(docs));
        clearSignal(admin);
        forceSheddingFailpoint(admin, false);
    });

    after(function () {
        MongoRunner.stopMongod(conn);
    });

    // Opens an idle cursor as 'owner' in an explicit session and waits for the sweep to shed it.
    function openAndShedOwnedCursor(ownerSession) {
        const ownerDb = ownerSession.getDatabase("test");
        const res = assert.commandWorked(
            ownerDb.runCommand({
                aggregate: jsTestName(),
                pipeline: kGroupPipeline,
                allowDiskUse: false,
                cursor: {batchSize: 1},
            }),
        );
        const cursorId = res.cursor.id;
        assert.neq(0, cursorId);

        const shedBefore = memStats(admin).loadShedding.idleCursorsShed;
        setSignal(admin, 95);
        forceSheddingFailpoint(admin, true);
        assert.soon(
            () => memStats(admin).loadShedding.idleCursorsShed > shedBefore,
            "idleCursorsShed did not increment",
            10 * 1000,
            kSweepMillis / 2,
        );
        forceSheddingFailpoint(admin, false);
        clearSignal(admin);
        return cursorId;
    }

    function ownerGetMore(ownerSession, cursorId) {
        return ownerSession
            .getDatabase("test")
            .runCommand({getMore: cursorId, collection: jsTestName(), batchSize: 1});
    }

    it("another user's getMore sees CursorNotFound and does not consume the reason", function () {
        const ownerConn = new Mongo(conn.host);
        assert(ownerConn.getDB("admin").auth("owner", "pwd"));
        const ownerSession = ownerConn.startSession();
        const otherConn = new Mongo(conn.host);
        assert(otherConn.getDB("admin").auth("other", "pwd"));
        const otherSession = otherConn.startSession();
        try {
            const cursorId = openAndShedOwnedCursor(ownerSession);

            // A different user, in their own session, cannot see why the cursor went away.
            assert.commandFailedWithCode(
                otherSession
                    .getDatabase("test")
                    .runCommand({getMore: cursorId, collection: jsTestName(), batchSize: 1}),
                ErrorCodes.CursorNotFound,
            );

            // The owner still gets the shed reason, exactly once.
            assert.commandFailedWithCode(
                ownerGetMore(ownerSession, cursorId),
                ErrorCodes.QueryMemoryLimitExceeded,
            );
            assert.commandFailedWithCode(
                ownerGetMore(ownerSession, cursorId),
                ErrorCodes.CursorNotFound,
            );
        } finally {
            forceSheddingFailpoint(admin, false);
            clearSignal(admin);
            ownerSession.endSession();
            otherSession.endSession();
        }
    });

    it("the owner in a different session sees CursorNotFound and does not consume the reason", function () {
        const ownerConn = new Mongo(conn.host);
        assert(ownerConn.getDB("admin").auth("owner", "pwd"));
        const ownerSession = ownerConn.startSession();
        const otherSession = ownerConn.startSession();
        try {
            const cursorId = openAndShedOwnedCursor(ownerSession);

            assert.commandFailedWithCode(
                otherSession
                    .getDatabase("test")
                    .runCommand({getMore: cursorId, collection: jsTestName(), batchSize: 1}),
                ErrorCodes.CursorNotFound,
            );

            assert.commandFailedWithCode(
                ownerGetMore(ownerSession, cursorId),
                ErrorCodes.QueryMemoryLimitExceeded,
            );
        } finally {
            forceSheddingFailpoint(admin, false);
            clearSignal(admin);
            ownerSession.endSession();
            otherSession.endSession();
        }
    });
});
