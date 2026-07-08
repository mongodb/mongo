/**
 * Verifies the kill-and-resume seam for change stream v2 precise shard targeting: disabling the
 * IFR flag 'featureFlagChangeStreamReaderV2' on mongos retires an already-open v2 cursor at
 * its next getMore with a resumable RetryChangeStream error. The stream then resumes from its token
 * on the v1 path.
 *
 * @tags: [
 *   assumes_balancer_off,
 *   requires_fcv_90,
 *   requires_sharding,
 *   uses_change_streams,
 * ]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {
    assertNoV2StageStateTransitionFrom,
    awaitLogMessageCodes,
    ChangeStreamTest,
    V2TargeterLogCodes,
} from "jstests/libs/query/change_stream_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const ifrFlagName = "featureFlagChangeStreamReaderV2";
const kInitStrictMode = V2TargeterLogCodes.kCollOrDbShardTargeterInitStrictMode;

describe("change-stream v2", function () {
    let st;
    const dbName = jsTestName();
    const collName = "coll";

    before(function () {
        st = new ShardingTest({
            shards: 1,
            mongos: 1,
            rs: {nodes: 1},
            mongosOptions: {
                setParameter: {
                    writePeriodicNoops: true,
                    periodicNoopIntervalSecs: 1,
                    // The v2 targeter's init and state-transition logs are debug-3 messages; raise
                    // query verbosity on mongos so we can observe (and assert the absence of) them.
                    logComponentVerbosity: tojson({query: {verbosity: 3}}),
                },
            },
        });

        const mongosDb = st.s.getDB(dbName);
        assert.commandWorked(
            mongosDb.adminCommand({enableSharding: dbName, primaryShard: st.rs0.getURL()}),
        );
        assert.commandWorked(
            mongosDb.adminCommand({shardCollection: `${dbName}.${collName}`, key: {_id: 1}}),
        );
    });

    after(function () {
        st.stop();
    });

    it("shard targeting resumes on IFR flag disable by killing the v2 cursor and resuming on the v1 path", function () {
        const db = st.s.getDB(dbName);
        const coll = db[collName];
        assert.commandWorked(coll.insert({_id: 1, v: 0}));

        const csTest = new ChangeStreamTest(db);
        const cursor = csTest.startWatchingChanges({
            pipeline: [{$changeStream: {version: "v2"}}],
            collection: coll,
            aggregateOptions: {cursor: {batchSize: 0}},
        });

        // The IFR flag is on by default (release phase), so the stream opens on v2. The v2 reader
        // initializes lazily on the first getMore; drive getMores until its strict-mode init log
        // appears on mongos, proving the cursor opened on the v2 path.
        awaitLogMessageCodes(st.s, [kInitStrictMode], () => csTest.assertNoChange(cursor));
        const cursorIdV2 = cursor.id;

        // Produce and consume an update on the v2 path; this advances the cursor's resume token.
        const expectedUpdate = (v) => ({
            operationType: "update",
            ns: {db: dbName, coll: collName},
            documentKey: {_id: 1},
            updateDescription: {updatedFields: {v}, removedFields: [], truncatedArrays: []},
        });
        assert.commandWorked(coll.update({_id: 1}, {$set: {v: 1}}));
        csTest.assertNextChangesEqual({cursor, expectedChanges: [expectedUpdate(1)]});

        // Disable the IFR flag on mongos. The v2 topology stage's per-getMore check now fires.
        assert.commandWorked(st.s.adminCommand({setParameter: 1, [ifrFlagName]: false}));

        // Produce another change so the next getMore has something to pump.
        assert.commandWorked(coll.update({_id: 1}, {$set: {v: 2}}));

        // A raw getMore on the still-open v2 cursor must throw the resumable RetryChangeStream
        // error. (A raw command is used here deliberately: ChangeStreamTest.getNextBatch would
        // transparently auto-resume on a resumable error, masking the exact code under test.)
        assert.soon(() => {
            const res = db.runCommand({getMore: cursor.id, collection: collName});
            if (res.ok) {
                return false; // event not surfaced yet; keep polling
            }
            assert.eq(res.code, ErrorCodes.RetryChangeStream, {res});
            return true;
        }, "v2 cursor was not retired with RetryChangeStream after disabling the IFR flag");

        // Resume from the last token. With the flag off, the stream reopens on v1.
        const logOffsetAtResume = checkLog.getGlobalLog(st.s).length;
        csTest.restartChangeStream(cursor); // mutates 'cursor' and asserts its id changed
        const cursorIdV1 = cursor.id;
        assert.neq(
            cursorIdV1,
            cursorIdV2,
            "The cursor ids must be different as the change stream has been closed and reopened",
        );

        // The resumed v1 stream still delivers the v:2 update.
        csTest.assertNextChangesEqual({cursor, expectedChanges: [expectedUpdate(2)]});

        // Confirm v1: the v2 topology stage never ran on the resumed stream (no state transitions).
        assertNoV2StageStateTransitionFrom(st.s, logOffsetAtResume, "Uninitialized");
    });
});
