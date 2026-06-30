/**
 * Tests that change stream error counters in serverStatus are incremented correctly
 * when corresponding error conditions occur on change stream cursors.
 *
 * @tags: [
 *   assumes_against_mongod_not_mongos,
 *   assumes_read_preference_unchanged,
 *   change_stream_does_not_expect_txns,
 *   uses_change_streams,
 *   requires_fcv_90,
 * ]
 */
import {after, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {
    assertDropAndRecreateCollection,
    assertDropCollection,
} from "jstests/libs/collection_drop_recreate.js";
import {ServerStatusMetrics} from "jstests/change_streams/change_stream_metrics_util.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";

const testDB = db.getSiblingDB(jsTestName());
const testColl = testDB.getCollection("test");

before(function () {
    assertDropAndRecreateCollection(testDB, testColl.getName());
});

after(function () {
    assertDropCollection(testDB, testColl.getName());
});

describe("change stream error counters are present in serverStatus", function () {
    it("all error counters have numeric values", function () {
        const err = ServerStatusMetrics.getCsErrorMetrics(db);

        // Counters are Int64 in BSON; use >= 0 rather than typeof == "number".
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

describe("non-retriable change stream error counters", function () {
    let errBefore;
    beforeEach(function () {
        errBefore = ServerStatusMetrics.getCsErrorMetrics(db);
    });

    it("nonRetriable.changeStreamFatalError increments when the pipeline removes the _id field", function () {
        // Projecting out _id strips the resume token from every change event, triggering a
        // ChangeStreamFatalError on the first event observed.
        const csCursor = testColl.watch([{$project: {_id: 0}}]);
        assert.commandWorked(testColl.insertOne({a: 1}));
        assert.throws(() =>
            assert.soon(
                () => csCursor.hasNext(),
                "expected change stream to throw ChangeStreamFatalError",
            ),
        );

        const errAfter = ServerStatusMetrics.getCsErrorMetrics(db);
        assert.gte(
            errAfter.nonRetriable.changeStreamFatalError,
            errBefore.nonRetriable.changeStreamFatalError + 1,
            "expected nonRetriable.changeStreamFatalError to increment by at least 1",
            {errBefore, errAfter},
        );
    });

    it("nonRetriable.bsonObjectTooLarge increments when a change event exceeds the BSON size limit", function () {
        const largeColl = testDB.getCollection("large_events");
        assertDropAndRecreateCollection(testDB, largeColl.getName());

        // The internal BSON size limit (BSONObjMaxInternalSize) is 16MB + 16KB.
        // A 16MB fullDocument alone leaves the event ~16MB + ~300 bytes, which is still under
        // the limit. An $addFields stage adds ~17KB to every change event, pushing the total
        // reliably over the limit.
        const kMaxBsonSize = 16 * 1024 * 1024;
        const kLargeStringLen = kMaxBsonSize - bsonsize({_id: "x", a: "x"}) + 1;
        const kExtraDataLen = 17000;
        // Open the cursor before inserting so the insert's change event is observed.
        const csCursor = largeColl.watch([{$addFields: {_extraData: "z".repeat(kExtraDataLen)}}]);

        assert.commandWorked(largeColl.insertOne({_id: "x", a: "y".repeat(kLargeStringLen)}));

        assert.throwsWithCode(
            () => assert.soon(() => csCursor.hasNext()),
            ErrorCodes.BSONObjectTooLarge,
        );

        assertDropCollection(testDB, largeColl.getName());

        const errAfter = ServerStatusMetrics.getCsErrorMetrics(db);
        assert.gte(
            errAfter.nonRetriable.bsonObjectTooLarge,
            errBefore.nonRetriable.bsonObjectTooLarge + 1,
            "expected nonRetriable.bsonObjectTooLarge to increment by at least 1",
            {errBefore, errAfter},
        );
    });

    it("nonRetriable.other increments when a non-retriable unnamed error escapes the pipeline", function () {
        const csCursor = testColl.watch();

        const fp = configureFailPoint(db, "throwErrorBeforeGetNext", {code: ErrorCodes.BadValue});
        try {
            // assert.soon is needed because hasNext() on a tailable cursor may return false
            // before dispatching a getMore that will trigger the failpoint.
            assert.throwsWithCode(() => assert.soon(() => csCursor.hasNext()), ErrorCodes.BadValue);
        } finally {
            fp.off();
        }

        const errAfter = ServerStatusMetrics.getCsErrorMetrics(db);
        assert.gte(
            errAfter.nonRetriable.other,
            errBefore.nonRetriable.other + 1,
            "expected nonRetriable.other to increment by at least 1",
            {errBefore, errAfter},
        );
    });

    it("error counters do not increment for non-change-stream getMore errors", function () {
        // Open a regular (non-change-stream) aggregation cursor. batchSize:0 defers result
        // delivery to the first getMore, exercising the command-layer guard.
        const cursor = testColl.aggregate([], {cursor: {batchSize: 0}});

        const fp = configureFailPoint(db, "throwErrorBeforeGetNext", {code: ErrorCodes.BadValue});
        try {
            // The failpoint is scoped to kChangeStream pipelines and does not fire here,
            // so the cursor drains successfully without incrementing any error counter.
            cursor.toArray();
        } finally {
            fp.off();
        }

        const errAfter = ServerStatusMetrics.getCsErrorMetrics(db);
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
            {
                errBefore,
                errAfter,
            },
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

describe("retriable change stream error counters", function () {
    let errBefore;
    beforeEach(function () {
        errBefore = ServerStatusMetrics.getCsErrorMetrics(db);
    });

    it("retriable.interruptedDueToReplStateChange increments when that error escapes the pipeline", function () {
        const csCursor = testColl.watch();

        const fp = configureFailPoint(db, "throwErrorBeforeGetNext", {
            code: ErrorCodes.InterruptedDueToReplStateChange,
        });
        try {
            assert.throwsWithCode(
                () => assert.soon(() => csCursor.hasNext()),
                ErrorCodes.InterruptedDueToReplStateChange,
            );
        } finally {
            fp.off();
        }

        const errAfter = ServerStatusMetrics.getCsErrorMetrics(db);
        assert.gte(
            errAfter.retriable.interruptedDueToReplStateChange,
            errBefore.retriable.interruptedDueToReplStateChange + 1,
            "expected retriable.interruptedDueToReplStateChange to increment by at least 1",
            {errBefore, errAfter},
        );
    });

    it("retriable.other increments when a retriable unnamed error escapes the pipeline", function () {
        const csCursor = testColl.watch();

        const fp = configureFailPoint(db, "throwErrorBeforeGetNext", {
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

        const errAfter = ServerStatusMetrics.getCsErrorMetrics(db);
        assert.gte(
            errAfter.retriable.other,
            errBefore.retriable.other + 1,
            "expected retriable.other to increment by at least 1",
            {errBefore, errAfter},
        );
    });
});
