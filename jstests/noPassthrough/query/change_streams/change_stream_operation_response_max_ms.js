/**
 * Tests that the 'operationResponseMaxMS' server parameter interrupts lengthy oplog scans in change
 * stream queries, causing getMore requests to return partial results with advancing post-batch
 * resume tokens (PBRTs) before the oplog tail is reached.
 *
 * The test slows down the oplog scan with the 'hangCollScanDoWork' failpoint configured to sleep a
 * fixed amount on every CollectionScan::doWork() call. Because each doWork() is delayed, the scan
 * reads several oplog entries before the 'operationResponseMaxMS' deadline fires and interrupts it.
 * Each interrupted getMore returns an empty batch (the watched collection has no events) with a PBRT
 * that reflects the oplog position reached before the interruption.
 *
 * Delaying every doWork() call - rather than blocking a single, fixed call - is deliberate:
 * 'hangCollScanDoWork' is a global failpoint, so the number of doWork() calls that precede the first
 * post-resume oplog entry (cursor creation, oplog-visibility yields, and unrelated background
 * collection scans) is not deterministic. A per-call delay guarantees the scan advances past the
 * resume-boundary entry before the deadline fires, so the PBRT reliably advances.
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {withFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {runWithParamsAllNonConfigNodes} from "jstests/noPassthrough/libs/server_parameter_helpers.js";

describe("operationResponseMaxMS parameter", () => {
    const kScanMaxMS = 1000;

    // The getMore blocks until the response deadline fires and then returns. Allow generous slack
    // for slow machines (e.g. TSAN-instrumented builds); the important property is that it returns
    // well before 'kGetMoreMaxTimeMS', which is the fallback when the deadline does not fire.
    const kGetMoreMaxTimeMS = 30000;
    const kMaxGetMoreDurationMS = kGetMoreMaxTimeMS / 2;

    let rst;
    let primary;
    let testDB;

    before(() => {
        rst = new ReplSetTest({
            nodes: 1,
            nodeOptions: {setParameter: {internalOperationResponseMaxMS: kScanMaxMS}},
        });
        rst.startSet();
        rst.initiate();

        primary = rst.getPrimary();
        testDB = primary.getDB(jsTestName());
    });

    after(() => {
        rst.stopSet();
    });

    it("returns multiple getMore responses with advancing PBRTs when the scan is interrupted", () => {
        const watchedCollName = "watched";
        const unrelatedColl = testDB.getCollection("unrelated");

        // Create the watched collection before opening the change stream so the DDL oplog entry
        // is already behind the stream's start position.
        assert.commandWorked(testDB.createCollection(watchedCollName));

        // Open a change stream on the watched collection. Use batchSize:0 so the initial response
        // contains no documents; the cursor's postBatchResumeToken reflects the current oplog tail.
        const aggRes = assert.commandWorked(
            testDB.runCommand({
                aggregate: watchedCollName,
                pipeline: [{$changeStream: {}}],
                cursor: {batchSize: 0},
            }),
        );

        const initialPBRT = aggRes.cursor.postBatchResumeToken;
        assert(initialPBRT, "Expected initial PBRT in aggregate response", {aggRes});

        const cursorId = aggRes.cursor.id;
        assert.neq(NumberLong(0), cursorId, "Expected an open change stream cursor", {aggRes});

        // Insert documents into an unrelated collection. These generate oplog entries with a
        // namespace that does not match the watched collection. Each getMore will scan these
        // entries, find no matching change events, and - when interrupted by the timeout -
        // return an empty batch whose PBRT has advanced past the last scanned entry.
        const kNumDocs = 500;
        assert.commandWorked(
            unrelatedColl.insertMany(Array.from({length: kNumDocs}, (_, i) => ({_id: i}))),
        );

        // Runs a getMore whose oplog scan is slowed by delaying every CollectionScan::doWork()
        // call by 'kDoWorkDelayMS'. The accumulated delay drives the scan past 'kScanMaxMS', so
        // the 'operationResponseMaxMS' deadline interrupts it after it has read several oplog
        // entries, and the getMore returns an empty batch with an advanced PBRT. The getMore
        // self-interrupts once the deadline is reached, so it can be issued inline without a
        // background thread.
        const kDoWorkDelayMS = 50;
        const runSlowGetMore = (cId) => {
            return withFailPoint(
                primary,
                "hangCollScanDoWork",
                () => {
                    const start = Date.now();
                    const res = assert.commandWorked(
                        testDB.runCommand({
                            getMore: cId,
                            collection: watchedCollName,
                            maxTimeMS: kGetMoreMaxTimeMS,
                        }),
                    );
                    // The 'internalOperationResponseMaxMS' deadline must fire even while the scan is
                    // waiting for new oplog entries, so the getMore returns close to the configured
                    // deadline instead of blocking until 'kGetMoreMaxTimeMS'.
                    assert.lt(
                        Date.now() - start,
                        kMaxGetMoreDurationMS,
                        "getMore must return shortly after the configured response deadline",
                    );
                    return res;
                },
                {data: {delay: kDoWorkDelayMS}},
            );
        };

        // First getMore: the scan is interrupted after reading several oplog entries from
        // unrelatedColl. The batch is empty (no matching events), but the PBRT has advanced.
        const res1 = runSlowGetMore(cursorId);

        assert.eq(
            0,
            res1.cursor.nextBatch.length,
            "Expected empty batch: watched collection has no change events",
            {
                res1,
            },
        );

        const pbrt1 = res1.cursor.postBatchResumeToken;
        assert(pbrt1, "Expected PBRT in first getMore response", {res1});
        assert.gt(
            bsonWoCompare(pbrt1, initialPBRT),
            0,
            "PBRT must advance after a partial oplog scan",
            {
                initialPBRT,
                pbrt1,
            },
        );

        assert.eq(
            res1.cursor.id,
            cursorId,
            "Cursor must remain open after a timeout-interrupted scan",
            {res1},
        );

        // Insert a second batch so the second getMore continues to have unscanned oplog entries
        // past the position where the first getMore was interrupted.
        assert.commandWorked(
            unrelatedColl.insertMany(
                Array.from({length: kNumDocs}, (_, i) => ({_id: kNumDocs + i})),
            ),
        );

        // Second getMore: the scan resumes from where it was interrupted, reads further oplog
        // entries from unrelatedColl, and is interrupted again. The PBRT advances further.
        const res2 = runSlowGetMore(cursorId);

        assert.eq(
            0,
            res2.cursor.nextBatch.length,
            "Expected empty batch from second partial scan",
            {res2},
        );

        const pbrt2 = res2.cursor.postBatchResumeToken;
        assert(pbrt2, "Expected PBRT in second getMore response", {res2});
        assert.gt(
            bsonWoCompare(pbrt2, pbrt1),
            0,
            "PBRT must advance further in second partial scan",
            {pbrt1, pbrt2},
        );

        assert.eq(
            cursorId,
            res2.cursor.id,
            "Cursor must remain open after second timeout-interrupted scan",
            {res2},
        );

        // Clean up the change stream cursor.
        assert.commandWorked(
            testDB.runCommand({killCursors: watchedCollName, cursors: [cursorId]}),
        );
    });

    // This case isolates the wait-for-inserts behavior. The change stream is idle at the oplog tail
    // (no backlog and no slow-scan failpoint), so the only thing that can bound the getMore is the
    // response deadline applied to the awaitData wait. Without that bounding, the getMore would
    // block for the full awaitData timeout ('kGetMoreMaxTimeMS') and this test would fail.
    it("returns the getMore near the response deadline while waiting at an idle oplog tail", () => {
        const watchedCollName = "watchedIdle";

        // Create the watched collection before opening the stream so the DDL oplog entry is already
        // behind the stream's start position.
        assert.commandWorked(testDB.createCollection(watchedCollName));

        // Open a change stream. batchSize:0 so the initial response contains no documents and the
        // cursor's postBatchResumeToken reflects the current oplog tail.
        const aggRes = assert.commandWorked(
            testDB.runCommand({
                aggregate: watchedCollName,
                pipeline: [{$changeStream: {}}],
                cursor: {batchSize: 0},
            }),
        );

        const cursorId = aggRes.cursor.id;
        assert.neq(NumberLong(0), cursorId, "Expected an open change stream cursor", {aggRes});

        // With no pending events the scan reaches the oplog tail immediately and blocks waiting for
        // inserts. The response deadline must bound that wait, so the getMore returns near
        // 'kScanMaxMS' instead of the awaitData timeout.
        const start = Date.now();
        const res = assert.commandWorked(
            testDB.runCommand({
                getMore: cursorId,
                collection: watchedCollName,
                maxTimeMS: kGetMoreMaxTimeMS,
            }),
        );
        const elapsed = Date.now() - start;

        assert.eq(0, res.cursor.nextBatch.length, "Expected empty batch at the idle tail", {
            res,
        });
        assert.lt(
            elapsed,
            kMaxGetMoreDurationMS,
            "getMore must return near the response deadline, not the awaitData timeout",
            {elapsed},
        );

        // Clean up the change stream cursor.
        assert.commandWorked(
            testDB.runCommand({killCursors: watchedCollName, cursors: [cursorId]}),
        );
    });

    // This case exercises the interaction between the response deadline and a selective downstream
    // filter. The watched collection has a backlog of events that pass the change stream's oplog
    // filter (so the cursor stage produces a non-empty batch), but a downstream $match rejects every
    // event. Once the deadline fires, the cursor stage must not keep scanning just because the
    // rejected batch left it with no results to return.
    it("bounds the getMore when a selective downstream filter rejects the batch", () => {
        const watchedCollName = "watchedFiltered";

        assert.commandWorked(testDB.createCollection(watchedCollName));

        // Disable periodic yielding before the filtered change stream is opened. The yield policy
        // captures 'internalQueryExecYieldPeriodMS' when the executor is constructed (at aggregate
        // time), so setting it afterwards would have no effect on this cursor. With periodic
        // yielding disabled, the response deadline can only be observed at the batch boundary.
        runWithParamsAllNonConfigNodes(testDB, {internalQueryExecYieldPeriodMS: 100000}, () => {
            // The $match rejects every change event, so the pipeline produces no results even though
            // the oplog scan reads matching entries. Use the equivalent regex rather than an equality
            // predicate: the change stream rewrite pushes 'operationType: "delete"' into the oplog
            // filter as 'op: "d"', which would filter out the inserted events before they could fill
            // the cursor's batches. A regex cannot be rewritten, so the events reach this $match and
            // are rejected downstream, which is the behavior this case exercises.
            const aggRes = assert.commandWorked(
                testDB.runCommand({
                    aggregate: watchedCollName,
                    pipeline: [
                        {$changeStream: {}},
                        {$match: {operationType: {$regex: "^delete$"}}},
                    ],
                    cursor: {batchSize: 0},
                }),
            );

            const cursorId = aggRes.cursor.id;
            assert.neq(NumberLong(0), cursorId, "Expected an open change stream cursor", {aggRes});

            // Insert a backlog of events into the watched collection. They pass the change stream's
            // oplog filter but are rejected by the downstream $match.
            const kNumDocs = 500;
            const watchedColl = testDB.getCollection(watchedCollName);
            assert.commandWorked(
                watchedColl.insertMany(Array.from({length: kNumDocs}, (_, i) => ({_id: i}))),
            );

            // Slow the scan so it runs past the response deadline.
            const kDoWorkDelayMS = 50;
            withFailPoint(
                primary,
                "hangCollScanDoWork",
                () => {
                    const start = Date.now();
                    const res = assert.commandWorked(
                        testDB.runCommand({
                            getMore: cursorId,
                            collection: watchedCollName,
                            maxTimeMS: kGetMoreMaxTimeMS,
                        }),
                    );
                    const elapsed = Date.now() - start;

                    // All events are rejected downstream, so the batch is empty. The deadline
                    // must still bound the getMore instead of letting it keep scanning.
                    assert.eq(
                        0,
                        res.cursor.nextBatch.length,
                        "Expected empty batch: all events rejected",
                        {res},
                    );
                    assert.lt(
                        elapsed,
                        kMaxGetMoreDurationMS,
                        "getMore must return near the response deadline even when a downstream filter rejects the batch",
                        {elapsed},
                    );
                },
                {data: {delay: kDoWorkDelayMS}},
            );

            // Clean up the change stream cursor.
            assert.commandWorked(
                testDB.runCommand({killCursors: watchedCollName, cursors: [cursorId]}),
            );
        });
    });
});
