/**
 * Tests that a change stream opted in with 'showErrorsAsEvents' reports lost oplog history as a
 * resumable operationType:"error" resume-gap event, rather than failing with
 * ChangeStreamHistoryLost, when the position it is opened at has rolled off the oplog of a replica
 * set, and that an open stream whose position rolls off fails with a resumable error.
 *
 * Rolling over the oplog is not covered for a sharded cluster through a router yet, as it requires
 * targeting the oplog of each shard. The test rolls over the oplog of the node it is connected to,
 * which is the primary of a replica set or, with a direct connection, of a shard.
 *
 * @tags: [
 *   featureFlagChangeStreamEmitErrorEvents,
 *   assumes_against_mongod_not_mongos,
 *   assumes_read_preference_unchanged,
 *   requires_majority_read_concern,
 *   requires_replication,
 *   uses_change_streams,
 * ]
 */
import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {after, afterEach, before, describe, it} from "jstests/libs/mochalite.js";
import {
    isDisagg,
    skipTestIfSizeBasedOplogTruncationDisabled,
} from "jstests/libs/oplog_truncation_util.js";
import {
    assertGetMoreFailsWithExpectedError,
    ChangeStreamTest,
    isResumableChangeStreamError,
    rollOplogPast,
} from "jstests/libs/query/change_stream_util.js";
import {getFirstOplogEntry, getLatestOp} from "jstests/replsets/rslib.js";

// This test relies on marker-based oplog truncation, which may be disabled in disagg.
// TODO(SERVER-125068) remove this once this feature flag is deleted
skipTestIfSizeBasedOplogTruncationDisabled(db.getMongo(), () => {});

const kOptIn = {showErrorsAsEvents: true};

// The retention is only reported when it is non-zero.
function getOplogMinRetentionHours(conn) {
    const res = assert.commandWorked(conn.adminCommand({serverStatus: 1}));
    return res.oplogTruncation?.oplogMinRetentionHours ?? 0;
}

// The ways to open a stream at a position given by an event.
const kStartPositions = [
    {name: "resumeAfter", specFromEvent: (event) => ({resumeAfter: event._id}), hasToken: true},
    {name: "startAfter", specFromEvent: (event) => ({startAfter: event._id}), hasToken: true},
    {
        name: "startAtOperationTime",
        specFromEvent: (event) => ({startAtOperationTime: event.clusterTime}),
        // The gap is addressed by a high-water-mark token for the time, which is not known.
        hasToken: false,
    },
];

describe("A change stream which shows errors as events", function () {
    const primary = db.getMongo();
    const testDB = db.getSiblingDB(jsTestName());
    let testColl;
    let fillerColl;
    let cst;
    let shardingState;
    let originalRetentionHours;
    let loweredRetention = false;

    before(function () {
        shardingState = assert.commandWorked(primary.adminCommand({shardingState: 1}));
        // Disagg retains 24h of oplog by default, so the oplog would never roll over in the test.
        originalRetentionHours = getOplogMinRetentionHours(primary);
        loweredRetention = isDisagg(primary) && originalRetentionHours > 0;
        if (loweredRetention) {
            assert.commandWorked(
                primary.adminCommand({replSetResizeOplog: 1, minRetentionHours: 0.000001}),
            );
        }
        testColl = assertDropAndRecreateCollection(testDB, "test");
        // The oplog is rolled with writes to another collection, so that none of the rolling writes
        // is an event of the streams under test.
        fillerColl = assertDropAndRecreateCollection(testDB, "filler");
        cst = new ChangeStreamTest(testDB);
    });

    afterEach(function () {
        cst.cleanUp();
    });

    after(function () {
        try {
            testDB.dropDatabase();
        } finally {
            if (loweredRetention) {
                assert.commandWorked(
                    primary.adminCommand({
                        replSetResizeOplog: 1,
                        minRetentionHours: originalRetentionHours,
                    }),
                );
            }
        }
    });

    // The event expected for an insert of {_id: id}.
    function insertEvent(id) {
        return {
            documentKey: {_id: id},
            fullDocument: {_id: id},
            ns: {db: testDB.getName(), coll: testColl.getName()},
            operationType: "insert",
        };
    }

    describe("opened at a position which rolled off the oplog", function () {
        let positionEvent;
        let earliestOplogTimestamp;

        before(function () {
            const stream = cst.startWatchingChanges({
                pipeline: [{$changeStream: kOptIn}],
                collection: testColl.getName(),
            });
            assert.commandWorked(testColl.insert({_id: 1}));
            assert.commandWorked(testColl.insert({_id: 2}));
            [, positionEvent] = cst.assertNextChangesEqual({
                cursor: stream,
                expectedChanges: [insertEvent(1), insertEvent(2)],
            });

            // Roll the whole oplog over, so the position's entry is no longer present.
            rollOplogPast(primary, fillerColl, getLatestOp(primary).ts);
            earliestOplogTimestamp = getFirstOplogEntry(primary, {readConcern: "majority"}).ts;
            assert.gt(earliestOplogTimestamp, positionEvent.clusterTime);
        });

        for (const {name, specFromEvent, hasToken} of kStartPositions) {
            it(`reports lost history as a resume gap with ${name}`, function () {
                const stream = cst.startWatchingChanges({
                    pipeline: [{$changeStream: {...kOptIn, ...specFromEvent(positionEvent)}}],
                    aggregateOptions: {cursor: {batchSize: 0}},
                    collection: testColl.getName(),
                });
                const expectedGap = {
                    operationType: "error",
                    clusterTime: positionEvent.clusterTime,
                    error: {
                        source: "resume-gap",
                        code: ErrorCodes.ChangeStreamHistoryLost,
                        codeName: "ChangeStreamHistoryLost",
                        message: "resume point predates the earliest available oplog entry",
                        earliestOplogTimestamp: earliestOplogTimestamp,
                        skippedClusterTimeRange: {
                            from: positionEvent.clusterTime,
                            to: earliestOplogTimestamp,
                        },
                    },
                };
                if (shardingState.enabled) {
                    // A shard names itself in the gap, whereas a replica set has no 'shardId'.
                    expectedGap.error.shardId = shardingState.shardName;
                }
                if (hasToken) {
                    // The event is addressed by the client's requested resume token.
                    expectedGap._id = positionEvent._id;
                }
                cst.assertNextChangesEqual({cursor: stream, expectedChanges: [expectedGap]});
            });
        }
    });

    it("fails an open stream whose position rolled off with a resumable error", function () {
        const stream = cst.startWatchingChanges({
            pipeline: [{$changeStream: kOptIn}],
            collection: testColl.getName(),
        });
        assert.commandWorked(testColl.insert({_id: "open"}));
        cst.assertNextChangesEqual({cursor: stream, expectedChanges: [insertEvent("open")]});

        rollOplogPast(primary, fillerColl, getLatestOp(primary).ts);

        // The rollover is not reported as an event: the driver reopens the stream from its last
        // token, which then reports the gap.
        const failure = assertGetMoreFailsWithExpectedError({
            db: testDB,
            cursorId: stream.id,
            collName: testColl.getName(),
            expectedError: (error) => error.code === ErrorCodes.RetryChangeStream,
            msg: "Expected the getMore to fail with RetryChangeStream after the rollover",
        });
        assert(isResumableChangeStreamError(failure), "Expected a resumable error", {failure});
        assert(!failure.hasOwnProperty("cursor"), "Expected no error event", {failure});
    });
});
