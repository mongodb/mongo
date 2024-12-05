// Test temporary output _id conflict stash collection resolution behaviour for resharding
// timeseries.
// @tags: [
//   requires_fcv_80,
// ]
//

import {ReshardingTest} from "jstests/sharding/libs/resharding_test_fixture.js";
const ns = "reshardingDb.coll";

const reshardingTest = new ReshardingTest({numDonors: 2, numRecipients: 1, reshardInPlace: false});
reshardingTest.setup();

const donorShardNames = reshardingTest.donorShardNames;
const recipientShardNames = reshardingTest.recipientShardNames;

const timeseriesInfo = {
    timeField: 'ts',
    metaField: 'meta'
};

const timeseriesCollection = reshardingTest.createShardedCollection({
    ns: ns,
    shardKeyPattern: {'meta.x': 1},
    chunks: [
        {min: {'meta.x': MinKey}, max: {'meta.x': 0}, shard: donorShardNames[0]},
        {min: {'meta.x': 0}, max: {'meta.x': MaxKey}, shard: donorShardNames[1]},
    ],
    collOptions: {
        timeseries: timeseriesInfo,
    }
});

const bucket2 = {
    data: 3,
    ts: new Date(),
    meta: {x: 2, y: 2}
};

// Create two buckets one on each donor.
assert.commandWorked(timeseriesCollection.insert([
    {data: 1, ts: new Date(), meta: {x: -2, y: 1}},
    bucket2,
]));

const bucketNss = "reshardingDb.system.buckets.coll";
const bucketsColl = reshardingTest._st.s.getDB("reshardingDb").getCollection('system.buckets.coll');

reshardingTest.withReshardingInBackground(
    {
        newShardKeyPattern: {'meta.y': 1},
        newChunks:
            [{min: {'meta.y': MinKey}, max: {'meta.y': MaxKey}, shard: recipientShardNames[0]}],
    },
    () => {
        const mongos = timeseriesCollection.getMongo();
        assert.soon(() => {
            const coordinatorDoc =
                mongos.getCollection("config.reshardingOperations").findOne({ns: bucketNss});
            return coordinatorDoc !== null && coordinatorDoc.cloneTimestamp !== undefined;
        });

        // Change bucket2's _id to match bucket1's _id.
        const newId = bucketsColl.findOne({'meta.x': -2})._id;
        const replacementBucket = bucketsColl.findOne({'meta.x': 2});
        const oldId = replacementBucket._id;
        replacementBucket._id = newId;
        bucketsColl.remove({_id: oldId});
        // This will add bucket2 to the stash collection.
        bucketsColl.insert(replacementBucket);

        // This will replicate an oplog delete entry. Which should delete bucket1 and cause bucket2
        // to get moved from stash to temporary resharding collection. Leading to an empty stash.
        bucketsColl.remove({'meta.x': -2});
    });

// Make sure bucket2 exists in the resharded collection.
const res = bucketsColl.find({}).toArray();
assert.eq(res.length, 1);
assert.eq(res[0].meta, bucket2.meta);

reshardingTest.teardown();
