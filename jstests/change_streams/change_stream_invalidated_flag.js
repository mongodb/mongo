/**
 * A change stream cursor response must report 'cursor.invalidated: true' once the stream
 * has been invalidated.
 *
 * @tags: [
 *   uses_change_streams,
 *   # A whole-cluster change stream can never be invalidated, so this test does not run in
 *   # the whole-cluster passthrough.
 *   do_not_run_in_whole_cluster_passthrough,
 *   requires_fcv_91,
 * ]
 */
import {
    assertDropAndRecreateCollection,
    assertDropCollection,
} from "jstests/libs/collection_drop_recreate.js";
import {describe, it, beforeEach, afterEach} from "jstests/libs/mochalite.js";

const testDB = db.getSiblingDB(jsTestName());
const kCollName = jsTestName();
const kBatchSizes = [undefined, 0, 1, 2, 10];

/**
 * Opens a change stream on the (already existing) collection, drops the database to invalidate
 * the stream, then drains the cursor with raw getMore commands. Returns the reply that closed
 * the cursor (cursor id 0).
 */
function drainUntilInvalidated(batchSize) {
    const aggRes = assert.commandWorked(
        testDB.runCommand({
            aggregate: kCollName,
            pipeline: [{$changeStream: {}}],
            cursor: {batchSize: 0},
        }),
    );
    let cursorId = aggRes.cursor.id;
    assert.neq(cursorId.valueOf(), 0, "expected an open change stream cursor", {aggRes});

    // Drop the database to invalidate the stream. This invalidates a collection-level stream and
    // is the only thing that invalidates a whole-db stream.
    assert.commandWorked(testDB.dropDatabase());

    let finalReply = null;
    let sawInvalidateEvent = false;
    assert.soon(() => {
        const cmd = {getMore: cursorId, collection: kCollName};
        if (batchSize !== undefined) {
            cmd.batchSize = batchSize;
        }
        const reply = assert.commandWorked(testDB.runCommand(cmd));
        if (reply.cursor.nextBatch.some((e) => e.operationType === "invalidate")) {
            sawInvalidateEvent = true;
        }
        cursorId = reply.cursor.id;
        if (cursorId.valueOf() === 0) {
            finalReply = reply;
            return true;
        }
        return false;
    });

    assert(sawInvalidateEvent, "expected to observe an invalidate event", {finalReply});
    assert(finalReply, "expected the cursor to be closed", {finalReply});
    assert.eq(finalReply.cursor.id.valueOf(), 0, "expected the cursor id to be zero", {finalReply});
    return finalReply;
}

describe("change stream invalidated flag", function () {
    beforeEach(function () {
        assertDropAndRecreateCollection(testDB, kCollName);
    });

    afterEach(function () {
        assertDropCollection(testDB, kCollName);
    });

    for (const batchSize of kBatchSizes) {
        it(`reports cursor.invalidated for batchSize ${batchSize}`, function () {
            const reply = drainUntilInvalidated(batchSize);
            assert.eq(
                reply.cursor.invalidated,
                true,
                "expected the closing change stream response to set cursor.invalidated",
                {reply},
            );
        });
    }

    it("does not report cursor.invalidated for a non-invalidated change stream", function () {
        const aggRes = assert.commandWorked(
            testDB.runCommand({
                aggregate: kCollName,
                pipeline: [{$changeStream: {}}],
                cursor: {batchSize: 0},
            }),
        );
        let cursorId = aggRes.cursor.id;
        assert.neq(cursorId.valueOf(), 0, "expected an open change stream cursor", {aggRes});

        assert.commandWorked(testDB.getCollection(kCollName).insert({_id: 1}));

        assert.soon(() => {
            const reply = assert.commandWorked(
                testDB.runCommand({getMore: cursorId, collection: kCollName}),
            );
            cursorId = reply.cursor.id;
            assert.eq(reply.cursor.invalidated, undefined, "cursor.invalidated must be unset", {
                reply,
            });
            return reply.cursor.nextBatch.some((e) => e.operationType === "insert");
        });

        assert.commandWorked(testDB.runCommand({killCursors: kCollName, cursors: [cursorId]}));
    });
});
