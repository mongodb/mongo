// Tests that change streams and their update lookups obey the read preference specified by the
// user.
// @tags: [
//   requires_majority_read_concern,
//   requires_profiling,
//   uses_change_streams,
// ]
import {enableLocalReadLogs} from "jstests/libs/local_reads.js";
import {after, afterEach, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {
    profilerHasAtLeastOneMatchingEntryOrThrow,
    profilerHasSingleMatchingEntryOrThrow,
} from "jstests/libs/profiler.js";
import {
    withChangeStreamTest,
    observePostImageLookup,
} from "jstests/libs/query/change_stream_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

describe("change stream and update lookup read preference", function () {
    const dbName = jsTestName();
    let st;
    let db;
    let coll;

    // Opens an updateLookup change stream with the given read preference, applies 'update' to both
    // documents, and asserts that both the stream and its post-image update lookup ran on the node
    // selected by the read preference (the primary or the lone secondary of each shard).
    function runReadPreferenceTest({comment, readPreference, nodeUnderTest}) {
        const ns = coll.getFullName();
        withChangeStreamTest(db, function (cst) {
            const stream = cst.startWatchingChanges({
                collection: coll,
                pipeline: [{$changeStream: {fullDocument: "updateLookup"}}],
                aggregateOptions: {comment, $readPreference: readPreference},
            });

            assert.commandWorked(coll.update({_id: -1}, {$set: {updated: true}}));
            assert.commandWorked(coll.update({_id: 1}, {$set: {updated: true}}));

            // Consume both updates while observing where the post-image lookup ran on each shard.
            let changes;
            const lookupFn = () => (changes = cst.getNextChanges(stream, 2));
            const nodeObservationMap = observePostImageLookup({
                nodes: [st.rs0, st.rs1].map(nodeUnderTest),
                ns,
                comment,
                fn: lookupFn,
            });
            assert.eq(changes[0].fullDocument, {_id: -1, updated: true});
            assert.eq(changes[1].fullDocument, {_id: 1, updated: true});

            for (const [node, observation] of nodeObservationMap) {
                const nodeDB = node.getDB(dbName);

                // The change stream itself runs on the read-preference-selected node. There might be
                // more than one entry if we needed multiple getMores to retrieve the changes.
                // TODO SERVER-31650 We have to use 'originatingCommand' here and look for the getMore
                // because the initial aggregate will not show up.
                profilerHasAtLeastOneMatchingEntryOrThrow({
                    profileDB: nodeDB,
                    filter: {"originatingCommand.comment": comment},
                });

                // The post-image update lookup runs on the same node: either as a local read (counted
                // by localReadCount) or as a separately-routed 'aggregate' visible in the profiler.
                // TODO SERVER-129059 When the optimized local lookup is enabled for sharded clusters,
                // the post-image is read locally via an _id index scan with no routed 'aggregate';
                // assert obs.indexOpsDelta["_id_"] === 1 instead.
                if (observation.localReadCount === 0) {
                    // Filter out any profiler entries with a stale config - this can be the first read
                    // on this node with a readConcern specified, which enforces shard version.
                    profilerHasSingleMatchingEntryOrThrow({
                        profileDB: nodeDB,
                        filter: {
                            op: "command",
                            ns: ns,
                            "command.comment": comment,
                            "command.aggregate": coll.getName(),
                            errCode: {$ne: ErrorCodes.StaleConfig},
                        },
                    });
                }
            }
        });
    }

    before(function () {
        st = new ShardingTest({
            name: "change_stream_read_pref",
            shards: 2,
            rs: {
                nodes: 2,
                // Use a higher frequency for periodic noops to speed up the test.
                setParameter: {periodicNoopIntervalSecs: 1, writePeriodicNoops: true},
            },
        });

        db = st.s0.getDB(dbName);
        coll = db[jsTestName()];

        // Enable sharding on the test DB and ensure its primary is st.shard0.shardName.
        assert.commandWorked(
            db.adminCommand({
                enableSharding: db.getName(),
                primaryShard: st.rs0.getURL(),
            }),
        );

        // Shard the test collection on _id.
        assert.commandWorked(db.adminCommand({shardCollection: coll.getFullName(), key: {_id: 1}}));

        // Split the collection into 2 chunks: [MinKey, 0), [0, MaxKey].
        assert.commandWorked(db.adminCommand({split: coll.getFullName(), middle: {_id: 0}}));

        // Move the [0, MaxKey] chunk to st.shard1.shardName.
        assert.commandWorked(
            db.adminCommand({
                moveChunk: coll.getFullName(),
                find: {_id: 1},
                to: st.rs1.getURL(),
            }),
        );

        // Turn on the profiler and local-read logging on every node.
        for (let rs of [st.rs0, st.rs1]) {
            assert.commandWorked(rs.getPrimary().getDB(dbName).setProfilingLevel(2));
            assert.commandWorked(rs.getSecondary().getDB(dbName).setProfilingLevel(2));
            enableLocalReadLogs(rs.getPrimary());
            enableLocalReadLogs(rs.getSecondary());
        }
    });

    beforeEach(function () {
        // Write a document to each chunk.
        assert.commandWorked(coll.insert({_id: -1}, {writeConcern: {w: "majority"}}));
        assert.commandWorked(coll.insert({_id: 1}, {writeConcern: {w: "majority"}}));
    });

    afterEach(function () {
        // Drop all documents.
        assert.commandWorked(coll.deleteMany({}));
    });

    after(function () {
        st.stop();
    });

    it("targets the primary by default", function () {
        runReadPreferenceTest({
            comment: "change stream against primary",
            readPreference: {mode: "primary"},
            nodeUnderTest: (rs) => rs.getPrimary(),
        });
    });

    it("targets a secondary with readPreference 'secondary'", function () {
        runReadPreferenceTest({
            comment: "change stream against secondary",
            readPreference: {mode: "secondary"},
            nodeUnderTest: (rs) => rs.getSecondary(),
        });
    });
});
