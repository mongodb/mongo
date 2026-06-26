// Tests that the post image update lookup will use the simple collation to do shard targeting, but
// use the collection's default collation once it gets to the shards.
//
// @tags: [
//   expects_explicit_underscore_id_index,
//   requires_majority_read_concern,
//   uses_change_streams,
// ]
// Shard key index has collation, which is not compatible with $min/$max
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {after, afterEach, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {
    observePostImageLookup,
    withChangeStreamTest,
} from "jstests/libs/query/change_stream_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

describe("change stream update lookup collation and shard targeting", function () {
    const caseInsensitive = {locale: "en_US", strength: 2};
    let st;
    let db;
    let coll;
    let isRunningOptimizedUpdateLookup;
    let oldSkipCheckOrphans;

    // Asserts the expected events and which index each shard's post-image lookup used. Legacy targets
    // via the shard-key index (the case-insensitive default collation makes the simple-collation
    // documentKey lookup ineligible for '_id'); the optimized local lookup uses shard key to determine
    // local or remote lookup, but for local lookups always is using '_id' index (guaranteed uniqueness per shard).
    function consumeAndAssertPostImageLookup(cst, cursor, expectedEvents) {
        const eventNs = {db: db.getName(), coll: coll.getName()};
        const expectedChanges = expectedEvents.map((e) => ({
            operationType: "update",
            ns: eventNs,
            documentKey: e.documentKey,
            fullDocument: e.fullDocument,
        }));
        const nodeObservationMap = observePostImageLookup({
            nodes: [st.rs0.getPrimary(), st.rs1.getPrimary()],
            ns: coll.getFullName(),
            fn: () => cst.assertNextChangesEqual({cursor, expectedChanges}),
        });

        // Each shard owns one updated doc, so its lookup runs once there.
        for (const [node, observation] of nodeObservationMap) {
            const idDelta = observation.indexOpsDelta["_id_"] ?? 0;
            const shardKeyDelta = observation.indexOpsDelta["shardKey_1"] ?? 0;
            if (isRunningOptimizedUpdateLookup) {
                assert.eq(idDelta, 1, `${node} did not use _id index`);
                assert.eq(shardKeyDelta, 0, `${node} unexpectedly used shard-key index`);
            } else {
                assert.eq(shardKeyDelta, 1, `shard${node} did not use shard-key index`);
                assert.eq(idDelta, 0, `shard${node} unexpectedly used _id index`);
            }
        }
    }

    before(function () {
        oldSkipCheckOrphans = TestData.skipCheckOrphans;
        TestData.skipCheckOrphans = true;

        st = new ShardingTest({
            shards: 2,
            config: 1,
            rs: {
                nodes: 1,
                // Use a higher frequency for periodic noops to speed up the test.
                setParameter: {writePeriodicNoops: true, periodicNoopIntervalSecs: 1},
            },
        });

        db = st.s0.getDB(jsTestName());
        isRunningOptimizedUpdateLookup = FeatureFlagUtil.isPresentAndEnabled(
            st.s.getDB("admin"),
            "ChangeStreamOptimizedUpdateLookup",
        );

        // Ensure the test db primary is st.shard0.shardName.
        assert.commandWorked(
            db.adminCommand({
                enableSharding: db.getName(),
                primaryShard: st.rs0.getURL(),
            }),
        );

        coll = db[jsTestName()];

        assert.commandWorked(db.runCommand({create: coll.getName(), collation: caseInsensitive}));

        // Shard the test collection on 'shardKey'. The shard key must use the simple collation.
        assert.commandWorked(
            db.adminCommand({
                shardCollection: coll.getFullName(),
                key: {shardKey: 1},
                collation: {locale: "simple"},
            }),
        );

        // Split the collection into 2 chunks: [MinKey, "aBC"), ["aBC", MaxKey). Note that there will
        // be documents in each chunk that will have the same shard key according to the collection's
        // default collation, but not according to the simple collation (e.g. "abc" and "ABC").
        assert.commandWorked(
            db.adminCommand({split: coll.getFullName(), middle: {shardKey: "aBC"}}),
        );

        // Move the [MinKey, 'aBC') chunk to st.shard1.shardName.
        assert.commandWorked(
            db.adminCommand({
                moveChunk: coll.getFullName(),
                find: {shardKey: "ABC"},
                to: st.rs1.getURL(),
            }),
        );

        // Make sure that "ABC" and "abc" go to different shards - we rely on that to make sure the
        // _ids are unique on each shard.
        assert.lte(bsonWoCompare({shardKey: "ABC"}, {shardKey: "aBC"}), -1);
        assert.gte(bsonWoCompare({shardKey: "abc"}, {shardKey: "aBC"}), 1);
    });

    beforeEach(function () {
        // Write some documents to each chunk. Note that the _id is purposefully not unique, since we
        // know the update lookup will use both the _id and the shard key, and we want to make sure
        // it is only targeting a single shard. Also note that _id is a string, since we want to make
        // sure the _id index can only be used if we are using the collection's default collation.
        assert.commandWorked(coll.insert({_id: "abc_1", shardKey: "ABC"}));
        assert.commandWorked(coll.insert({_id: "abc_2", shardKey: "ABC"}));
        assert.commandWorked(coll.insert({_id: "abc_1", shardKey: "abc"}));
        assert.commandWorked(coll.insert({_id: "abc_2", shardKey: "abc"}));
    });

    afterEach(function () {
        assert.commandWorked(coll.deleteMany({}));
    });

    after(function () {
        st.stop();

        TestData.skipCheckOrphans = oldSkipCheckOrphans;
    });

    it("uses simple collation to target and the default collation to look up", function () {
        withChangeStreamTest(db, (cst) => {
            const cursor = cst.startWatchingChanges({
                collection: coll,
                pipeline: [{$changeStream: {fullDocument: "updateLookup"}}],
            });

            // Be sure to include the collation in the updates so that each can be targeted to exactly
            // one shard - this is important to ensure each update only updates one document (since
            // with the default collation their documentKeys are identical). If each operation updates
            // only one, the clusterTime sent from mongos will ensure that each corresponding oplog
            // entry has a distinct timestamp and so will appear in the change stream in the order we
            // expect.
            let updateResult = coll.updateOne(
                {shardKey: "abc", _id: "abc_1"},
                {$set: {updated: true}},
                {collation: {locale: "simple"}},
            );
            assert.eq(1, updateResult.modifiedCount);
            updateResult = coll.updateOne(
                {shardKey: "ABC", _id: "abc_1"},
                {$set: {updated: true}},
                {collation: {locale: "simple"}},
            );
            assert.eq(1, updateResult.modifiedCount);

            consumeAndAssertPostImageLookup(cst, cursor, [
                {
                    documentKey: {shardKey: "abc", _id: "abc_1"},
                    fullDocument: {shardKey: "abc", _id: "abc_1", updated: true},
                },
                {
                    documentKey: {shardKey: "ABC", _id: "abc_1"},
                    fullDocument: {shardKey: "ABC", _id: "abc_1", updated: true},
                },
            ]);
        });
    });

    it("uses simple collation to target even with a non-default stream collation", function () {
        // Strength 1 will consider "ç" equal to "c" and "C".
        const strengthOneCollation = {locale: "en_US", strength: 1};

        // Insert some documents that might be confused with existing documents under the change
        // stream's collation, but should not be confused during the update lookup.
        assert.commandWorked(coll.insert({_id: "abç_1", shardKey: "ABÇ"}));
        assert.commandWorked(coll.insert({_id: "abç_2", shardKey: "ABÇ"}));
        assert.commandWorked(coll.insert({_id: "abç_1", shardKey: "abç"}));
        assert.commandWorked(coll.insert({_id: "abç_2", shardKey: "abç"}));

        assert.eq(coll.find({shardKey: "abc"}).collation(strengthOneCollation).itcount(), 8);

        withChangeStreamTest(db, (cst) => {
            const cursor = cst.startWatchingChanges({
                collection: coll,
                pipeline: [
                    {$changeStream: {fullDocument: "updateLookup"}},
                    {$match: {"fullDocument.shardKey": "abc"}},
                ],
                aggregateOptions: {collation: strengthOneCollation},
            });

            let updateResult = coll.updateOne(
                {shardKey: "ABC", _id: "abc_1"},
                {$set: {updated: true}},
                {collation: {locale: "simple"}},
            );
            assert.eq(1, updateResult.modifiedCount);
            updateResult = coll.updateOne(
                {shardKey: "abc", _id: "abc_1"},
                {$set: {updated: true}},
                {collation: {locale: "simple"}},
            );
            assert.eq(1, updateResult.modifiedCount);

            consumeAndAssertPostImageLookup(cst, cursor, [
                {
                    documentKey: {shardKey: "ABC", _id: "abc_1"},
                    fullDocument: {shardKey: "ABC", _id: "abc_1", updated: true},
                },
                {
                    documentKey: {shardKey: "abc", _id: "abc_1"},
                    fullDocument: {shardKey: "abc", _id: "abc_1", updated: true},
                },
            ]);
        });
    });
});
