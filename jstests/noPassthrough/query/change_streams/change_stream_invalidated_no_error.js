/**
 * Tests that ChangeStreamInvalidated does not increment change stream error counters,
 * because invalidation is a normal cursor lifecycle event, not an error.
 *
 * This test must live in noPassthrough because the whole-cluster and whole-db passthrough
 * suites convert collection drops into drop events rather than invalidate events, which
 * would prevent this test from ever observing the invalidate event it expects.
 *
 * @tags: [
 *   requires_replication,
 *   uses_change_streams,
 *   requires_fcv_90,
 * ]
 */
import {after, describe, it} from "jstests/libs/mochalite.js";
import {
    assertDropAndRecreateCollection,
    assertDropCollection,
} from "jstests/libs/collection_drop_recreate.js";
import {ServerStatusMetrics} from "jstests/libs/query/change_stream_metrics_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();
const db = rst.getPrimary().getDB("test");
const testDB = db.getSiblingDB(jsTestName());

after(function () {
    rst.stopSet();
});

describe("ChangeStreamInvalidated is not an error", function () {
    it("nonRetriable.other does not increment when ChangeStreamInvalidated invalidates the cursor", function () {
        // ChangeStreamInvalidated is a normal lifecycle event (not an error): the getMore command
        // handler catches it explicitly and marks the cursor as invalidated without propagating the
        // exception, so incrementChangeStreamErrorCounters() is never called for it.
        const invalidateColl = testDB.getCollection("invalidate_test");
        assertDropAndRecreateCollection(testDB, invalidateColl.getName());

        const csCursor = invalidateColl.watch();

        const errBefore = ServerStatusMetrics.getCsErrorMetrics(db);

        // Dropping the collection triggers a ChangeStreamInvalidated exception inside the pipeline,
        // which the getMore handler converts into an invalidate event rather than an error.
        assertDropCollection(testDB, invalidateColl.getName());

        // Drain the cursor until we see the invalidate event (operationType == "invalidate").
        assert.soon(() => {
            if (csCursor.hasNext()) {
                const event = csCursor.next();
                if (event.operationType === "invalidate") {
                    return true;
                }
            }
            return !csCursor.isExhausted() ? false : true;
        }, "expected to receive an invalidate event after dropping the collection");

        const errAfter = ServerStatusMetrics.getCsErrorMetrics(db);

        // None of the error counters should have changed: ChangeStreamInvalidated is not an error.
        assert.eq(
            errBefore.nonRetriable.other,
            errAfter.nonRetriable.other,
            "nonRetriable.other must not increment for ChangeStreamInvalidated",
            {errBefore, errAfter},
        );
        assert.eq(
            errBefore.nonRetriable.changeStreamHistoryLost,
            errAfter.nonRetriable.changeStreamHistoryLost,
            "nonRetriable.changeStreamHistoryLost must not increment for ChangeStreamInvalidated",
            {errBefore, errAfter},
        );
        assert.eq(
            errBefore.nonRetriable.changeStreamFatalError,
            errAfter.nonRetriable.changeStreamFatalError,
            "nonRetriable.changeStreamFatalError must not increment for ChangeStreamInvalidated",
            {errBefore, errAfter},
        );
        assert.eq(
            errBefore.nonRetriable.bsonObjectTooLarge,
            errAfter.nonRetriable.bsonObjectTooLarge,
            "nonRetriable.bsonObjectTooLarge must not increment for ChangeStreamInvalidated",
            {errBefore, errAfter},
        );
        assert.eq(
            errBefore.retriable.interruptedDueToReplStateChange,
            errAfter.retriable.interruptedDueToReplStateChange,
            "retriable.interruptedDueToReplStateChange must not increment for ChangeStreamInvalidated",
            {errBefore, errAfter},
        );
        assert.eq(
            errBefore.retriable.other,
            errAfter.retriable.other,
            "retriable.other must not increment for ChangeStreamInvalidated",
            {errBefore, errAfter},
        );
    });
});
