/**
 * Tests that change stream cursors respect readPreference after replica set elections in a sharded
 * cluster. When a shard node's role changes (e.g., secondary becomes primary), getMore on a change
 * stream cursor with a strict readPreference (secondary, primary) should fail with a retryable
 * error, and the client can resume from the last resume token.
 *
 * @tags: [
 *   uses_change_streams,
 *   requires_sharding,
 *   requires_fcv_90,
 * ]
 */
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {awaitRSClientHosts} from "jstests/replsets/rslib.js";
import {describe, it, before, after, afterEach} from "jstests/libs/mochalite.js";
import {assertDropCollection} from "jstests/libs/collection_drop_recreate.js";
import {ChangeStreamTest, isResumableChangeStreamError} from "jstests/libs/query/change_stream_util.js";

/**
 * Polls getMore until the predicate matches, failing immediately on any error. Use this (not
 * csTest.assertNextChangesEqual) when the test must verify that no election error occurred —
 * assertNextChangesEqual retries on ResumableChangeStreamError and would mask such failures.
 */
function assertGetMoreSucceedsAfterElection(db, cursor, collName, predicate, msg) {
    assert.soon(() => {
        const r = assert.commandWorked(db.runCommand({getMore: cursor.id, collection: collName}));
        return r.cursor.nextBatch.some(predicate);
    }, msg);
}

/**
 * Polls getMore until it fails with one of the expected retryable error codes that indicate a
 * readPreference mismatch after an election. Through mongos the ARM may wrap the shard error as
 * HostUnreachable before the cursor is fully torn down.
 */
function assertGetMoreFailsAfterElectionWithRetryableError(db, cursorId, collName, msg) {
    assert.soon(function () {
        const r = db.runCommand({getMore: cursorId, collection: collName});
        if (r.ok === 0) {
            assert(isResumableChangeStreamError(r), "Unexpected error: " + tojson(r));
            return true;
        }
        return false;
    }, msg);
}

describe("change stream readPreference enforcement on sharded cluster elections", function () {
    let st;
    let mongosDb;
    let coll;
    let csTest;
    const dbName = jsTestName();
    const collName = "test";

    before(function () {
        st = new ShardingTest({
            shards: 1,
            rs: {
                nodes: 2,
                setParameter: {
                    periodicNoopIntervalSecs: 1,
                    writePeriodicNoops: true,
                },
            },
            configOptions: {
                setParameter: {writePeriodicNoops: true, periodicNoopIntervalSecs: 1},
            },
        });

        mongosDb = st.s.getDB(dbName);
        coll = mongosDb[collName];

        assert.commandWorked(
            mongosDb.adminCommand({enableSharding: mongosDb.getName(), primaryShard: st.rs0.getURL()}),
        );
        assert.commandWorked(mongosDb.adminCommand({shardCollection: coll.getFullName(), key: {_id: 1}}));
    });

    afterEach(function () {
        csTest.cleanUp();
        csTest = null;
        assertDropCollection(mongosDb, collName);
    });

    after(function () {
        st.stop();
    });

    function insertAndAssertEvents(csTest, cursor, coll, ns) {
        assert.commandWorked(coll.insertMany([{_id: -1}, {_id: 1}]));
        csTest.assertNextChangesEqualUnordered({
            cursor,
            expectedChanges: [
                {
                    operationType: "insert",
                    ns,
                    fullDocument: {_id: -1},
                    documentKey: {_id: -1},
                },
                {
                    operationType: "insert",
                    ns,
                    fullDocument: {_id: 1},
                    documentKey: {_id: 1},
                },
            ],
        });
    }

    describe("does fail", function () {
        it("fails with retryable error when shard secondary becomes primary (readPref: secondary)", function () {
            csTest = new ChangeStreamTest(mongosDb);
            const cursor = csTest.startWatchingChanges({
                collection: collName,
                pipeline: [{$changeStream: {}}],
                aggregateOptions: {$readPreference: {mode: "secondary"}},
            });
            const cursorId = cursor.id;

            insertAndAssertEvents(csTest, cursor, coll, {db: dbName, coll: collName});

            // Step up a secondary on shard0.
            const shard0Secondary = st.rs0.getSecondary();
            st.rs0.stepUp(shard0Secondary, {awaitReplicationBeforeStepUp: false});

            assertGetMoreFailsAfterElectionWithRetryableError(
                mongosDb,
                cursorId,
                collName,
                "Expected change stream getMore to fail after shard election",
            );
        });

        it("fails with retryable error when shard primary steps down (readPref: primary)", function () {
            csTest = new ChangeStreamTest(mongosDb);
            const cursor = csTest.startWatchingChanges({
                collection: collName,
                pipeline: [{$changeStream: {}}],
                aggregateOptions: {$readPreference: {mode: "primary"}},
            });
            const cursorId = cursor.id;

            insertAndAssertEvents(csTest, cursor, coll, {db: dbName, coll: collName});

            // Step up a secondary so the current shard0 primary steps down.
            st.rs0.stepUp(st.rs0.getSecondary(), {awaitReplicationBeforeStepUp: false});

            assertGetMoreFailsAfterElectionWithRetryableError(
                mongosDb,
                cursorId,
                collName,
                "Expected change stream getMore to fail after shard primary stepdown",
            );
        });

        it("resumes correctly after readPreference enforcement error", function () {
            csTest = new ChangeStreamTest(mongosDb);
            const cursor = csTest.startWatchingChanges({
                collection: collName,
                pipeline: [{$changeStream: {}}],
                aggregateOptions: {$readPreference: {mode: "secondary"}},
            });

            insertAndAssertEvents(csTest, cursor, coll, {db: dbName, coll: collName});

            const resumeToken = csTest.getResumeToken(cursor);

            // Step up a secondary on shard0 to trigger error on next cursor use.
            const shard0OldPrimary = st.rs0.getPrimary();
            const shard0NewPrimary = st.rs0.getSecondary();
            st.rs0.stepUp(shard0NewPrimary, {awaitReplicationBeforeStepUp: false});

            // Ensure secondary and primary are set before continuing.
            st.rs0.awaitNodesAgreeOnPrimary();
            awaitRSClientHosts(st.s, shard0NewPrimary, {ok: true, ismaster: true});
            awaitRSClientHosts(st.s, shard0OldPrimary, {ok: true, ismaster: false});

            // Insert new data after the election.
            assert.commandWorked(coll.insert({_id: -2}));

            // Resume from the last token — should see the new insert.
            const resumeCursor = csTest.startWatchingChanges({
                collection: collName,
                pipeline: [{$changeStream: {resumeAfter: resumeToken}}],
                aggregateOptions: {$readPreference: {mode: "secondary"}},
            });
            csTest.assertNextChangesEqual({
                cursor: resumeCursor,
                expectedChanges: [
                    {
                        operationType: "insert",
                        ns: {db: dbName, coll: collName},
                        fullDocument: {_id: -2},
                        documentKey: {_id: -2},
                    },
                ],
            });
        });
    });

    describe("does not fail", function () {
        it("for secondaryPreferred when shard secondary becomes primary", function () {
            csTest = new ChangeStreamTest(mongosDb);
            const cursor = csTest.startWatchingChanges({
                collection: collName,
                pipeline: [{$changeStream: {}}],
                aggregateOptions: {$readPreference: {mode: "secondaryPreferred"}},
            });

            insertAndAssertEvents(csTest, cursor, coll, {db: dbName, coll: collName});

            assert.commandWorked(coll.insert({_id: 2}));
            st.rs0.stepUp(st.rs0.getSecondary(), {awaitReplicationBeforeStepUp: false});

            assertGetMoreSucceedsAfterElection(
                mongosDb,
                cursor,
                collName,
                (e) => e.fullDocument._id === 2,
                "Expected event after election (secondaryPreferred)",
            );
        });

        it("for primaryPreferred when shard primary steps down", function () {
            csTest = new ChangeStreamTest(mongosDb);
            const cursor = csTest.startWatchingChanges({
                collection: collName,
                pipeline: [{$changeStream: {}}],
                aggregateOptions: {$readPreference: {mode: "primaryPreferred"}},
            });

            insertAndAssertEvents(csTest, cursor, coll, {db: dbName, coll: collName});

            assert.commandWorked(coll.insert({_id: 2}));
            st.rs0.stepUp(st.rs0.getSecondary(), {awaitReplicationBeforeStepUp: false});

            assertGetMoreSucceedsAfterElection(
                mongosDb,
                cursor,
                collName,
                (e) => e.fullDocument._id === 2,
                "Expected event after election (primaryPreferred)",
            );
        });

        it("for nearest when shard elections occur", function () {
            csTest = new ChangeStreamTest(mongosDb);
            const cursor = csTest.startWatchingChanges({
                collection: collName,
                pipeline: [{$changeStream: {}}],
                aggregateOptions: {$readPreference: {mode: "nearest"}},
            });

            insertAndAssertEvents(csTest, cursor, coll, {db: dbName, coll: collName});

            assert.commandWorked(coll.insert({_id: 2}));
            st.rs0.stepUp(st.rs0.getSecondary(), {awaitReplicationBeforeStepUp: false});

            assertGetMoreSucceedsAfterElection(
                mongosDb,
                cursor,
                collName,
                (e) => e.fullDocument._id === 2,
                "Expected event after election (nearest)",
            );
        });
    });
});
