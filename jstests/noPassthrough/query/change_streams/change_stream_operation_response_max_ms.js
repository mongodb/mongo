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
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

describe("operationResponseMaxMS parameter", () => {
    const kScanMaxMS = 1000;

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

        // Use an explicit session so the change stream cursor and its getMores share one lsid.
        const session = primary.startSession();
        const sessionDB = session.getDatabase(jsTestName());

        // Open a change stream on the watched collection. Use batchSize:0 so the initial
        // response contains no documents; the cursor's postBatchResumeToken reflects the current
        // oplog tail.
        const aggRes = assert.commandWorked(
            sessionDB.runCommand({
                aggregate: watchedCollName,
                pipeline: [{$changeStream: {}}],
                cursor: {batchSize: 0},
            }),
        );

        const initialPBRT = aggRes.cursor.postBatchResumeToken;
        assert(initialPBRT, "Expected initial PBRT in aggregate response", {aggRes});

        let cursorId = aggRes.cursor.id;
        assert.neq(NumberLong(0), cursorId, "Expected an open change stream cursor", {aggRes});

        // Insert documents into an unrelated collection. These generate oplog entries with a
        // namespace that does not match the watched collection. Each getMore will scan these
        // entries, find no matching change events, and — when interrupted by the timeout —
        // return an empty batch whose PBRT has advanced past the last scanned entry.
        const kNumDocs = 500;
        assert.commandWorked(
            unrelatedColl.insertMany(Array.from({length: kNumDocs}, (_, i) => ({_id: i}))),
        );

        // Runs a getMore whose oplog scan is slowed by delaying every CollectionScan::doWork()
        // call by 'kDoWorkDelayMS'. The accumulated delay drives the scan past 'kScanMaxMS', so the
        // 'operationResponseMaxMS' deadline interrupts it after it has read several oplog entries,
        // and the getMore returns an empty batch with an advanced PBRT. The getMore self-interrupts
        // once the deadline is reached, so it can be issued inline without a background thread.
        const kDoWorkDelayMS = 50;
        const runSlowGetMore = (cId) => {
            const fp = configureFailPoint(primary, "hangCollScanDoWork", {delay: kDoWorkDelayMS});
            try {
                return assert.commandWorked(
                    sessionDB.runCommand({
                        getMore: cId,
                        collection: watchedCollName,
                        maxTimeMS: 30000,
                    }),
                );
            } finally {
                fp.off();
            }
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

        // Clean up the change stream cursor and session.
        assert.commandWorked(
            sessionDB.runCommand({killCursors: watchedCollName, cursors: [cursorId]}),
        );
        session.endSession();
    });
});
