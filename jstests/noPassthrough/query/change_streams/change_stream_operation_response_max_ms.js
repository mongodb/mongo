/**
 * Tests that the 'operationResponseMaxMS' server parameter interrupts lengthy oplog scans in change
 * stream queries, causing getMore requests to return partial results with advancing post-batch
 * resume tokens (PBRTs) before the oplog tail is reached.
 *
 * The test simulates a slow oplog scan by using the 'hangCollScanDoWork' failpoint, which blocks
 * inside CollectionScan::doWork(). With the failpoint active for longer than operationResponseMaxMS,
 * the yield that fires after doWork() returns detects that the configured scan time limit has
 * been exceeded and interrupts the scan. Each such interrupted getMore returns an empty batch
 * (the watched collection has no events) with an updated PBRT that reflects the oplog position
 * reached before the interruption. A background thread issues each getMore so the main thread
 * can control the failpoint timing.
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {Thread} from "jstests/libs/parallelTester.js";

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

        // Use an explicit session so the same lsid can be passed to background threads.
        // getMore requires the same logical session as the cursor was created in.
        const session = primary.startSession();
        const lsidJson = tojson(session.getSessionId());
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
        assert.commandWorked(unrelatedColl.insertMany(Array.from({length: kNumDocs}, (_, i) => ({_id: i}))));

        // Issues a getMore in a background thread while the hangCollScanDoWork failpoint keeps
        // the oplog scan blocked inside CollectionScan::doWork() for longer than
        // kScanMaxMS. The failpoint uses {skip: 1} to let the first doWork() call through
        // freely (which processes the oplog entry at the stream's start position and may return
        // ADVANCED, bypassing the yield check). The second doWork() call — on the first insert
        // entry — is blocked by the failpoint for longer than kScanMaxMS. When the
        // failpoint is released, that call returns NEED_TIME and the yield check fires the
        // timeout, causing the getMore to complete with an empty batch and an updated PBRT.
        const runSlowGetMore = (cId) => {
            const fp = configureFailPoint(primary, "hangCollScanDoWork", {}, {skip: 1});

            // Serialize both the cursor ID and lsid via tojson so the Thread can reconstruct
            // them with eval. tojson preserves exact BSON types including BinData subtypes.
            const cursorIdJson = tojson(cId);
            const thread = new Thread(
                function (host, dbName, collName, lsidJson, cursorIdJson) {
                    const conn = new Mongo(host);
                    const db = conn.getDB(dbName);
                    return db.runCommand({
                        getMore: eval("(" + cursorIdJson + ")"),
                        collection: collName,
                        lsid: eval("(" + lsidJson + ")"),
                        maxTimeMS: 30000,
                    });
                },
                primary.host,
                testDB.getName(),
                watchedCollName,
                lsidJson,
                cursorIdJson,
            );

            thread.start();

            // Wait until the scan has entered doWork() and is blocked on the failpoint.
            fp.wait();

            // Sleep for longer than the configured limit. The pauseWhileSet() loop inside
            // doWork() polls the failpoint every ~200 ms, so add extra margin.
            sleep(kScanMaxMS + 500);

            // Unblock the scan. The yield that fires immediately after will detect that
            // kScanMaxMS has elapsed and interrupt the scan.
            fp.off();

            thread.join();
            return assert.commandWorked(thread.returnData());
        };

        // First getMore: the scan is interrupted after processing one oplog entry from
        // unrelatedColl. The batch is empty (no matching events), but the PBRT has advanced.
        const res1 = runSlowGetMore(cursorId);

        assert.eq(0, res1.cursor.nextBatch.length, "Expected empty batch: watched collection has no change events", {
            res1,
        });

        const pbrt1 = res1.cursor.postBatchResumeToken;
        assert(pbrt1, "Expected PBRT in first getMore response", {res1});
        assert.gt(bsonWoCompare(pbrt1, initialPBRT), 0, "PBRT must advance after a partial oplog scan", {
            initialPBRT,
            pbrt1,
        });

        assert.eq(res1.cursor.id, cursorId, "Cursor must remain open after a timeout-interrupted scan", {res1});

        // Insert a second batch so the second getMore has new oplog entries to scan. The first
        // getMore's second loadBatch() consumed the initial inserts while draining to the oplog
        // tail after the timeout fired.
        assert.commandWorked(unrelatedColl.insertMany(Array.from({length: kNumDocs}, (_, i) => ({_id: kNumDocs + i}))));

        // Second getMore: the scan resumes from where it was interrupted, processes the next
        // oplog entry from unrelatedColl, and is interrupted again. The PBRT advances further.
        const res2 = runSlowGetMore(cursorId);

        assert.eq(0, res2.cursor.nextBatch.length, "Expected empty batch from second partial scan", {res2});

        const pbrt2 = res2.cursor.postBatchResumeToken;
        assert(pbrt2, "Expected PBRT in second getMore response", {res2});
        assert.gt(bsonWoCompare(pbrt2, pbrt1), 0, "PBRT must advance further in second partial scan", {pbrt1, pbrt2});

        assert.eq(cursorId, res2.cursor.id, "Cursor must remain open after second timeout-interrupted scan", {res2});

        // Clean up the change stream cursor and session.
        assert.commandWorked(sessionDB.runCommand({killCursors: watchedCollName, cursors: [cursorId]}));
        session.endSession();
    });
});
