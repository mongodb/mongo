/**
 * Tests that an updateLookup change stream converts a 'TooManyMatchingDocuments' error into a
 * clean ChangeStreamFatalError, instead of crashing or returning a wrong result.
 *
 * The '_id' field is only unique per-shard, not cluster-wide, for a collection sharded on a key
 * other than '_id'. A change event captured while the collection was still unsharded carries a
 * documentKey of {_id: ...} only. Replaying that event's updateLookup after the collection has
 * since been sharded on a different key can no longer target a single shard with that stale
 * documentKey, so the lookup falls back to a generic aggregation that scatters the match to every
 * shard. If two different shards each happen to own a real document with the same '_id', the
 * scattered match finds more than one document for the same documentKey.
 *
 * @tags: [
 *   uses_change_streams,
 * ]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {ChangeStreamTest, waitForClusterTime} from "jstests/libs/query/change_stream_util.js";

describe("change stream updateLookup TooManyMatchingDocuments", function () {
    let st;
    let mongosDB;
    let mongosColl;

    before(function () {
        st = new ShardingTest({
            shards: 2,
            mongos: 1,
            rs: {nodes: 1, setParameter: {writePeriodicNoops: true, periodicNoopIntervalSecs: 1}},
        });
        mongosDB = st.s.getDB(jsTestName());
        mongosColl = mongosDB.coll;
        assert.commandWorked(
            mongosDB.adminCommand({
                enableSharding: mongosDB.getName(),
                primaryShard: st.shard0.shardName,
            }),
        );
        assert.commandWorked(mongosColl.createIndex({b: 1}));
    });

    after(function () {
        st.stop();
    });

    it("converts a scattered duplicate-_id match into a ChangeStreamFatalError", function () {
        // Insert while the collection is still unsharded, so the update event's documentKey below
        // is {_id: 1} only, with no shard key to disambiguate it later.
        assert.commandWorked(mongosColl.insert({_id: 1, b: -100}));

        // Record the current cluster time so the change stream below can begin exactly at this point.
        const startTime = waitForClusterTime(mongosDB, st);

        assert.commandWorked(mongosColl.update({_id: 1}, {$set: {updated: true}}));

        // Shard on 'b' and split so the pre-existing document (b: -100) stays on shard0 while a
        // newly-inserted document with a positive 'b' lands on shard1.
        assert.commandWorked(
            mongosDB.adminCommand({shardCollection: mongosColl.getFullName(), key: {b: 1}}),
        );
        assert.commandWorked(
            mongosDB.adminCommand({split: mongosColl.getFullName(), middle: {b: 0}}),
        );
        assert.commandWorked(
            mongosDB.adminCommand({
                moveChunk: mongosColl.getFullName(),
                find: {b: 0},
                to: st.shard1.shardName,
            }),
        );

        // A brand-new document that happens to reuse _id: 1 on the other shard. _id uniqueness is
        // only enforced per-shard, not cluster-wide.
        assert.commandWorked(mongosColl.insert({_id: 1, b: 100}));

        // Starting a stream at 'startTime' replays the update event with its stale documentKey
        // ({_id: 1}), which can no longer be targeted to a single shard now that the collection is
        // sharded on 'b'. The lookup falls back to the generic aggregation path, scatters to both
        // shards, and finds a real _id: 1 document on each: TooManyMatchingDocuments, converted
        // to a clean ChangeStreamFatalError.
        ChangeStreamTest.assertChangeStreamThrowsCode({
            db: mongosDB,
            collName: mongosColl.getName(),
            pipeline: [
                {$changeStream: {startAtOperationTime: startTime, fullDocument: "updateLookup"}},
            ],
            expectedCode: ErrorCodes.ChangeStreamFatalError,
            validateExceptionDetails: (error) => {
                assert(
                    error.message.includes("found more than one document"),
                    "Got unexpected error message for TooManyMatchingDocuments case",
                    {error},
                );
            },
        });
    });
});
