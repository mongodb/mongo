/**
 * Reproduces a "missing invalidate after drop" bug when a change stream is resumed on a shard after
 * an invalidate event, and the next matching event on the shard is another invalidating event (e.g.
 * "drop", "dropDatabase").
 *
 * @tags: [assumes_balancer_off, does_not_support_stepdowns, uses_change_streams, requires_sharding]
 */
import {describe, it, before, after, afterEach} from "jstests/libs/mochalite.js";
import {assertDropCollection} from "jstests/libs/collection_drop_recreate.js";
import {ChangeStreamTest} from "jstests/libs/query/change_stream_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

describe("sharded change stream observes multiple invalidate events", () => {
    const kDBName = jsTestName();
    const kCollName = "test";

    let st;
    let csTest;

    before(() => {
        st = new ShardingTest({
            shards: 3,
            mongos: 1,
            config: 1,
            rs: {nodes: 1, setParameter: {writePeriodicNoops: true, periodicNoopIntervalSecs: 1}},
            other: {
                enableBalancer: false,
            },
        });
    });

    after(() => {
        st.stop();
    });

    afterEach(() => {
        st.s.getDB(kDBName).dropDatabase();
        if (csTest) {
            csTest.cleanUp();
            csTest = null;
        }
    });

    it("observes a different drop event directly after invalidate", () => {
        const db = st.s.getDB(kDBName);

        csTest = new ChangeStreamTest(db);
        let csCursor = csTest.startWatchingChanges({
            pipeline: [
                {
                    $changeStream: {showExpandedEvents: true},
                },
                {
                    $match: {operationType: {$nin: ["createIndexes", "dropIndexes"]}},
                },
            ],
            collection: kCollName,
        });

        csTest.assertNoChange(csCursor);

        const shards = assert.commandWorked(st.s.adminCommand({listShards: 1})).shards;

        // DB primary pinned to shard 0.
        assert.commandWorked(st.s.adminCommand({enableSharding: kDBName, primaryShard: shards[0].shardName}));

        // Create unsplittable collection on shard 0.
        assert.commandWorked(
            db.runCommand({
                createUnsplittableCollection: kCollName,
                dataShard: shards[0].shardName,
            }),
        );

        // Drop collection and recreate it on shard 1.
        assertDropCollection(db, kCollName);

        assert.commandWorked(
            db.runCommand({
                createUnsplittableCollection: kCollName,
                dataShard: shards[1].shardName,
            }),
        );

        assertDropCollection(db, kCollName);

        csTest.assertNextChangesEqual({
            cursor: csCursor,
            expectedChanges: [
                {
                    operationType: "create",
                    ns: {db: kDBName, coll: kCollName},
                    nsType: "collection",
                },
                {
                    operationType: "drop",
                    ns: {db: kDBName, coll: kCollName},
                },
                {
                    operationType: "invalidate",
                },
            ],
            expectInvalidate: true,
        });

        // Retrieve the resume token from the stream, and close the change stream.
        let resumeToken = csTest.getResumeToken(csCursor);
        csTest.cleanUp();
        csTest = null;

        // Open a new change stream from the invalidate event.
        csTest = new ChangeStreamTest(db);
        csCursor = csTest.startWatchingChanges({
            pipeline: [
                {
                    $changeStream: {showExpandedEvents: true, startAfter: resumeToken},
                },
                {
                    $match: {operationType: {$nin: ["createIndexes", "dropIndexes"]}},
                },
            ],
            collection: kCollName,
        });

        csTest.assertNextChangesEqual({
            cursor: csCursor,
            expectedChanges: [
                {
                    operationType: "create",
                    ns: {db: kDBName, coll: kCollName},
                    nsType: "collection",
                },
                {
                    operationType: "drop",
                    ns: {db: kDBName, coll: kCollName},
                },
                {
                    operationType: "invalidate",
                },
            ],
            expectInvalidate: true,
        });
    });
});
