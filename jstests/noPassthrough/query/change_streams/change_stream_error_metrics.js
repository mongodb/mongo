/**
 * Tests that change stream error counters in the mongoS serverStatus are incremented correctly
 * when corresponding error conditions occur on change stream cursors opened through mongos.
 *
 * Errors from shard pipelines travel back to mongos as non-OK StatusWith values via the
 * AsyncResultsMerger. ClusterClientCursorImpl::next() checks these returned statuses and
 * increments the appropriate per-process counter on mongoS.
 *
 * The counters on mongod (per-shard) and mongos (per-router) are independent. This test
 * verifies only the mongoS side.
 *
 * @tags: [
 *   requires_sharding,
 *   uses_change_streams,
 *   requires_fcv_90,
 *   # Change stream error counters are new in 9.0.
 * ]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {
    assertDropAndRecreateCollection,
    assertDropCollection,
} from "jstests/libs/collection_drop_recreate.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {ServerStatusMetrics} from "jstests/change_streams/change_stream_metrics_util.js";

let st, mongos, testDB, testColl;

before(function () {
    st = new ShardingTest({
        shards: 1,
        mongos: 1,
        rs: {nodes: 1, setParameter: {writePeriodicNoops: true, periodicNoopIntervalSecs: 1}},
    });
    mongos = st.s;
    testDB = mongos.getDB(jsTestName());
    testColl = testDB.getCollection("test");
    assertDropAndRecreateCollection(testDB, testColl.getName());
});

after(function () {
    assertDropCollection(testDB, testColl.getName());
    st.stop();
});

describe("change stream error counters are present in mongoS serverStatus", function () {
    it("all six error counters have numeric values", function () {
        const err = ServerStatusMetrics.getCsErrorMetrics(testDB);

        assert.gte(
            err.nonRetriable.changeStreamHistoryLost,
            0,
            "missing nonRetriable.changeStreamHistoryLost",
        );
        assert.gte(
            err.nonRetriable.changeStreamFatalError,
            0,
            "missing nonRetriable.changeStreamFatalError",
        );
        assert.gte(
            err.nonRetriable.bsonObjectTooLarge,
            0,
            "missing nonRetriable.bsonObjectTooLarge",
        );
        assert.gte(err.nonRetriable.other, 0, "missing nonRetriable.other");
        assert.gte(
            err.retriable.interruptedDueToReplStateChange,
            0,
            "missing retriable.interruptedDueToReplStateChange",
        );
        assert.gte(err.retriable.other, 0, "missing retriable.other");
    });
});

describe("non-retriable change stream error counters on mongoS", function () {
    it("nonRetriable.changeStreamFatalError increments when pipeline removes the _id field", function () {
        const errBefore = ServerStatusMetrics.getCsErrorMetrics(testDB);

        // Projecting out _id strips the resume token, which triggers ChangeStreamFatalError on
        // the shard. The error propagates to mongos as a non-OK Status via the ARM.
        const csCursor = testColl.watch([{$project: {_id: 0}}]);
        assert.commandWorked(testColl.insertOne({a: 1}));
        assert.throws(() =>
            assert.soon(
                () => csCursor.hasNext(),
                "expected change stream to throw ChangeStreamFatalError",
            ),
        );

        const errAfter = ServerStatusMetrics.getCsErrorMetrics(testDB);
        assert.gte(
            errAfter.nonRetriable.changeStreamFatalError,
            errBefore.nonRetriable.changeStreamFatalError + 1,
            "expected nonRetriable.changeStreamFatalError to increment by at least 1",
            {errBefore, errAfter},
        );
    });

    it("nonRetriable.changeStreamHistoryLost increments when the oplog is exhausted", function () {
        const csCursor = testColl.watch();
        const errBefore = ServerStatusMetrics.getCsErrorMetrics(testDB);

        const fp = configureFailPoint(st.rs0.getPrimary(), "throwErrorBeforeGetNext", {
            code: ErrorCodes.ChangeStreamHistoryLost,
        });
        try {
            assert.throwsWithCode(
                () => assert.soon(() => csCursor.hasNext()),
                ErrorCodes.ChangeStreamHistoryLost,
            );
        } finally {
            fp.off();
        }

        const errAfter = ServerStatusMetrics.getCsErrorMetrics(testDB);
        assert.gte(
            errAfter.nonRetriable.changeStreamHistoryLost,
            errBefore.nonRetriable.changeStreamHistoryLost + 1,
            "expected nonRetriable.changeStreamHistoryLost to increment by at least 1",
            {errBefore, errAfter},
        );
    });

    it("nonRetriable.bsonObjectTooLarge increments when a document exceeds the BSON size limit", function () {
        const csCursor = testColl.watch();
        const errBefore = ServerStatusMetrics.getCsErrorMetrics(testDB);

        const fp = configureFailPoint(st.rs0.getPrimary(), "throwErrorBeforeGetNext", {
            code: ErrorCodes.BSONObjectTooLarge,
        });
        try {
            assert.throwsWithCode(
                () => assert.soon(() => csCursor.hasNext()),
                ErrorCodes.BSONObjectTooLarge,
            );
        } finally {
            fp.off();
        }

        const errAfter = ServerStatusMetrics.getCsErrorMetrics(testDB);
        assert.gte(
            errAfter.nonRetriable.bsonObjectTooLarge,
            errBefore.nonRetriable.bsonObjectTooLarge + 1,
            "expected nonRetriable.bsonObjectTooLarge to increment by at least 1",
            {errBefore, errAfter},
        );
    });

    it("nonRetriable.other increments when a non-retriable unnamed error escapes the pipeline", function () {
        const csCursor = testColl.watch();
        const errBefore = ServerStatusMetrics.getCsErrorMetrics(testDB);

        const fp = configureFailPoint(st.rs0.getPrimary(), "throwErrorBeforeGetNext", {
            code: ErrorCodes.BadValue,
        });
        try {
            assert.throwsWithCode(() => assert.soon(() => csCursor.hasNext()), ErrorCodes.BadValue);
        } finally {
            fp.off();
        }

        const errAfter = ServerStatusMetrics.getCsErrorMetrics(testDB);
        assert.gte(
            errAfter.nonRetriable.other,
            errBefore.nonRetriable.other + 1,
            "expected nonRetriable.other to increment by at least 1",
            {errBefore, errAfter},
        );
    });
});

describe("retriable change stream error counters on mongoS", function () {
    it("retriable.interruptedDueToReplStateChange increments on repl state change errors", function () {
        const csCursor = testColl.watch();
        const errBefore = ServerStatusMetrics.getCsErrorMetrics(testDB);

        const fp = configureFailPoint(st.rs0.getPrimary(), "throwErrorBeforeGetNext", {
            code: ErrorCodes.InterruptedDueToReplStateChange,
        });
        try {
            // InterruptedDueToReplStateChange is in NotPrimaryError, so the change stream
            // machinery treats it as a resumable signal and retries internally. Each retry
            // passes through ClusterClientCursorImpl::next() and increments the counter
            // before the retry is attempted. The cursor eventually fails with a different
            // error after the retries are exhausted — we don't assert on the final code.
            try {
                assert.soon(() => csCursor.hasNext());
            } catch (e) {
                // expected: retries exhausted, cursor fails with a different error code
            }
        } finally {
            fp.off();
        }

        const errAfter = ServerStatusMetrics.getCsErrorMetrics(testDB);
        assert.gte(
            errAfter.retriable.interruptedDueToReplStateChange,
            errBefore.retriable.interruptedDueToReplStateChange + 1,
            "expected retriable.interruptedDueToReplStateChange to increment by at least 1",
            {errBefore, errAfter},
        );
    });

    it("retriable.other increments when a retriable unnamed error escapes the pipeline", function () {
        const csCursor = testColl.watch();
        const errBefore = ServerStatusMetrics.getCsErrorMetrics(testDB);

        const fp = configureFailPoint(st.rs0.getPrimary(), "throwErrorBeforeGetNext", {
            code: ErrorCodes.NetworkTimeout,
        });
        try {
            assert.throwsWithCode(
                () => assert.soon(() => csCursor.hasNext()),
                ErrorCodes.NetworkTimeout,
            );
        } finally {
            fp.off();
        }

        const errAfter = ServerStatusMetrics.getCsErrorMetrics(testDB);
        assert.gte(
            errAfter.retriable.other,
            errBefore.retriable.other + 1,
            "expected retriable.other to increment by at least 1",
            {errBefore, errAfter},
        );
    });
});

describe("mongoS error counters do not increment for non-change-stream errors", function () {
    it("error counters do not increment for non-change-stream getMore errors", function () {
        // Open a regular (non-change-stream) aggregation cursor via mongos.
        const cursor = testColl.aggregate([], {cursor: {batchSize: 0}});
        const errBefore = ServerStatusMetrics.getCsErrorMetrics(testDB);

        const fp = configureFailPoint(st.rs0.getPrimary(), "throwErrorBeforeGetNext", {
            code: ErrorCodes.BadValue,
        });
        try {
            // The failpoint only fires for change stream pipelines (ResumableScanType::kChangeStream)
            // so the regular aggregation drains successfully without incrementing any counter.
            cursor.toArray();
        } finally {
            fp.off();
        }

        const errAfter = ServerStatusMetrics.getCsErrorMetrics(testDB);
        assert.eq(
            errBefore.nonRetriable.changeStreamHistoryLost,
            errAfter.nonRetriable.changeStreamHistoryLost,
            "unexpected increment",
            {errBefore, errAfter},
        );
        assert.eq(
            errBefore.nonRetriable.changeStreamFatalError,
            errAfter.nonRetriable.changeStreamFatalError,
            "unexpected increment",
            {errBefore, errAfter},
        );
        assert.eq(
            errBefore.nonRetriable.bsonObjectTooLarge,
            errAfter.nonRetriable.bsonObjectTooLarge,
            "unexpected increment",
            {errBefore, errAfter},
        );
        assert.eq(
            errBefore.nonRetriable.other,
            errAfter.nonRetriable.other,
            "unexpected increment",
            {errBefore, errAfter},
        );
        assert.eq(
            errBefore.retriable.interruptedDueToReplStateChange,
            errAfter.retriable.interruptedDueToReplStateChange,
            "unexpected increment",
            {errBefore, errAfter},
        );
        assert.eq(errBefore.retriable.other, errAfter.retriable.other, "unexpected increment", {
            errBefore,
            errAfter,
        });
    });
});
