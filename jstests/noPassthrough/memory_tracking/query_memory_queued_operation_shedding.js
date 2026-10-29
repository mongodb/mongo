/**
 * This tests the server's ability to shed operations that are queued waiting for an execution
 * ticket.
 *
 * Setup: a single read ticket. A find parked in the shardWaitInFindBeforeMakingBatch failpoint
 * holds that ticket. A getMore on a $group cursor that already accumulated ~10 MB of tracked
 * memory then queues behind it. Under failpoint-simulated memory pressure the sweep must kill the
 * queued getMore and must leave the running ticket holder alone. With load shedding disabled, the
 * queued getMore is expected to survive until the ticket frees up.
 *
 * @tags: [
 *   requires_fcv_91,
 *   assumes_against_mongod_not_mongos,
 * ]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {checkLog} from "src/mongo/shell/check_log.js";
import {getEngine} from "jstests/libs/query/analyze_plan.js";

// The cursor is opened in this shell and iterated from a parallel shell, which is only allowed when
// neither is bound to a logical session.
TestData.disableImplicitSessions = true;

// Spill thresholds high enough that the $group never spills.
const kHuge = 512 * 1024 * 1024;
// kNumDocs * kValBytes ~= 10 MB of tracked $group memory held by the open cursor.
const kNumDocs = 20000;
const kValBytes = 512;
const kGroupPipeline = [{$group: {_id: "$g", vals: {$push: "$val"}}}];

function loadSheddingStats(db) {
    return assert.commandWorked(db.adminCommand({serverStatus: 1})).queryMemory?.loadShedding;
}

function setParam(db, params) {
    assert.commandWorked(db.adminCommand(Object.assign({setParameter: 1}, params)));
}

// Opens a $group cursor with batchSize 1 so the cursor keeps its ~10 MB of tracked memory while
// idle between getMores.
function openGroupCursor(coll) {
    const cursor = coll.aggregate(kGroupPipeline, {allowDiskUse: false, cursor: {batchSize: 1}});
    assert.gt(cursor.objsLeftInBatch(), 0);
    return cursor.getId();
}

function queuedReaders(db) {
    return assert.commandWorked(db.adminCommand({serverStatus: 1})).globalLock.currentQueue.readers;
}

describe("queued-operation query-memory load shedding", function () {
    let conn;
    let db;
    let coll;

    // A getMore on an aggregation cursor only queues behind a held read ticket under SBE. Classic
    // agg cursors are kLocksInternally: their getMores neither restore shard-role acquisitions nor
    // re-acquire an execution ticket, so there is nothing for the queued-operation sweep to shed.
    // Under the classic engine, assert that a getMore is not blocked by the held ticket instead, so
    // that this test fails if classic getMores ever start queueing.
    function itPerEngine(title, sbeFn) {
        it(title, function () {
            const framework = assert.commandWorked(
                db.adminCommand({getParameter: 1, internalQueryFrameworkControl: 1}),
            ).internalQueryFrameworkControl;
            if (framework === "forceClassicEngine") {
                assertClassicGetMoreNotQueued();
                return;
            }
            assert.eq(
                "sbe",
                getEngine(coll.explain().aggregate(kGroupPipeline)),
                "The group pipeline no longer runs under SBE, so the queued-operation sweep " +
                    "premise is broken",
            );
            sbeFn();
        });
    }

    function assertClassicGetMoreNotQueued() {
        const cursorId = openGroupCursor(coll);
        withTicketHeld(() => {
            const waitForGetMore = startParallelShell(
                funWithArgs(
                    function (collName, cursorId) {
                        assert.commandWorked(
                            db.getSiblingDB("test").runCommand({
                                getMore: cursorId,
                                collection: collName,
                                batchSize: 1,
                                comment: "classic getMore",
                            }),
                        );
                    },
                    coll.getName(),
                    cursorId,
                ),
                conn.port,
            );
            // The getMore must complete while the ticket is still held.
            assert.soon(
                () =>
                    db
                        .getSiblingDB("admin")
                        .aggregate([
                            {$currentOp: {}},
                            {$match: {"command.comment": "classic getMore"}},
                        ])
                        .itcount() === 0,
                "classic getMore was blocked by the held read ticket",
            );
            assert.eq(0, queuedReaders(db), "classic getMore should not queue for a ticket");
            waitForGetMore();
        });
    }

    before(function () {
        conn = MongoRunner.runMongod({
            setParameter: {
                queryMemoryLoadSheddingLowMarkPercent: 50,
                queryMemoryLoadSheddingHighMarkPercent: 60,
                queryMemoryLoadSheddingSizeReferenceBytes: 1024,
                queryMemoryRssMonitorIntervalMillis: 50,
                internalDocumentSourceGroupMaxMemoryBytes: kHuge,
                internalQuerySlotBasedExecutionHashAggApproxMemoryUseInBytesBeforeSpill: kHuge,
                // One read ticket, fixed, no deprioritization: the second reader always queues.
                executionControlConcurrencyAdjustmentAlgorithm: "fixedConcurrentTransactions",
                executionControlConcurrentReadTransactions: 1,
                executionControlDeprioritizationGate: false,
            },
        });
        assert.neq(null, conn, "mongod failed to start");
        db = conn.getDB("test");
        coll = db[jsTestName()];
        const docs = [];
        for (let i = 0; i < kNumDocs; i++) {
            docs.push({_id: i, g: i, val: "x".repeat(kValBytes)});
        }
        assert.commandWorked(coll.insertMany(docs));
    });

    after(function () {
        MongoRunner.stopMongod(conn);
    });

    // Runs 'body' while a find holds the only read ticket. Returns after the holder has exited.
    function withTicketHeld(body) {
        const holdFp = configureFailPoint(conn, "shardWaitInFindBeforeMakingBatch", {
            shouldCheckForInterrupt: true,
        });
        const waitForHolder = startParallelShell(
            funWithArgs(function (collName) {
                const res = db.getSiblingDB("test").runCommand({
                    find: collName,
                    filter: {_id: 0},
                    comment: "ticket holder",
                });
                assert.commandWorked(res);
            }, coll.getName()),
            conn.port,
        );
        try {
            holdFp.wait();
            body();
        } finally {
            holdFp.off();
            waitForHolder();
        }
    }

    itPerEngine(
        "a getMore blocked on a ticket is shed by the sweep; the ticket holder is not",
        function () {
            const cursorId = openGroupCursor(coll);
            const shedBefore = loadSheddingStats(db).operationsShed;

            withTicketHeld(() => {
                const waitForGetMore = startParallelShell(
                    funWithArgs(
                        function (collName, cursorId) {
                            const res = db.getSiblingDB("test").runCommand({
                                getMore: cursorId,
                                collection: collName,
                                batchSize: 1,
                                comment: "queued getMore",
                            });
                            assert.commandFailedWithCode(
                                res,
                                ErrorCodes.QueryMemoryLimitExceeded,
                                "queued getMore should have been shed: " + tojson(res),
                            );
                        },
                        coll.getName(),
                        cursorId,
                    ),
                    conn.port,
                );
                assert.soon(
                    () => queuedReaders(db) >= 1,
                    "getMore never queued for the read ticket",
                );

                // Above the high mark the shed probability is 1, so the sweep sheds on its second
                // observation. Its shed is counted in operationsShed like any other shed.
                const pressureFp = configureFailPoint(conn, "queryMemoryPressureOverride", {
                    usagePercent: 90,
                });
                try {
                    assert.soon(
                        () => loadSheddingStats(db).operationsShed > shedBefore,
                        "sweep never shed the queued getMore",
                    );
                    waitForGetMore();
                } finally {
                    pressureFp.off();
                }
                // The running ticket holder was never a candidate: it is still parked in its failpoint
                // holding the ticket, and its shell asserts commandWorked once released (below).
                assert.eq(0, queuedReaders(db), "shed getMore should have left the ticket queue");
                assert.gte(
                    assert.commandWorked(db.adminCommand({serverStatus: 1})).globalLock
                        .activeClients.readers,
                    1,
                    "ticket holder should still hold its ticket",
                );
            });
            // The shed log line marks the op as shed while waiting for a ticket.
            checkLog.containsJson(conn, 13033300, {waitingForTicket: true});
        },
    );

    itPerEngine(
        "with load shedding disabled, a queued getMore is admitted when the ticket frees",
        function () {
            // The loadShedding serverStatus section only exists while the feature is enabled, so
            // capture the baseline first.
            const shedBefore = loadSheddingStats(db).operationsShed;
            setParam(db, {queryMemoryLoadSheddingLowMarkPercent: -1});
            const cursorId = openGroupCursor(coll);
            let waitForGetMore;

            withTicketHeld(() => {
                waitForGetMore = startParallelShell(
                    funWithArgs(
                        function (collName, cursorId) {
                            // No checkpoint is reached while queued and shedding is off, so the getMore is
                            // admitted once the ticket frees up and cannot be shed at any point.
                            const res = db.getSiblingDB("test").runCommand({
                                getMore: cursorId,
                                collection: collName,
                                batchSize: 1,
                            });
                            assert.commandWorked(res, tojson(res));
                        },
                        coll.getName(),
                        cursorId,
                    ),
                    conn.port,
                );
                assert.soon(
                    () => queuedReaders(db) >= 1,
                    "getMore never queued for the read ticket",
                );
                const pressureFp = configureFailPoint(conn, "queryMemoryPressureOverride", {
                    usagePercent: 90,
                });
                // Give the disabled feature many intervals to misbehave.
                sleep(500);
                // While disabled there is no loadShedding serverStatus section at all.
                assert.eq(undefined, loadSheddingStats(db));
                assert.eq(1, queuedReaders(db), "getMore should still be queued");
                pressureFp.off();
            });
            // The ticket was released by withTicketHeld; the getMore proceeds and the shell exits.
            waitForGetMore();
            setParam(db, {queryMemoryLoadSheddingLowMarkPercent: 50});
            assert.eq(
                shedBefore,
                loadSheddingStats(db).operationsShed,
                "disabled shedding must not shed",
            );
        },
    );
});
