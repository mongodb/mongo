// Test nonempty temporary output _id conflict stash collection abort for resharding timeseries.
// @tags: [
//   requires_fcv_80,
// ]
//
import {getTimeseriesCollForDDLOps} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";
import {getRawOperationSpec, getTimeseriesCollForRawOps} from "jstests/libs/raw_operation_utils.js";
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

const coll = reshardingTest.createShardedCollection({
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
const db = coll.getDB();

// Create two buckets one on each donor.
assert.commandWorked(coll.insert([
    {data: 1, ts: new Date(), meta: {x: -2, y: 1}},
    {data: 3, ts: new Date(), meta: {x: 2, y: 2}},
]));

reshardingTest.withReshardingInBackground(
    {
        newShardKeyPattern: {'meta.y': 1},
        newChunks:
            [{min: {'meta.y': MinKey}, max: {'meta.y': MaxKey}, shard: recipientShardNames[0]}],
    },
    () => {
        const mongos = coll.getMongo();
        assert.soon(() => {
            const coordinatorDoc = mongos.getCollection("config.reshardingOperations").findOne({
                ns: getTimeseriesCollForDDLOps(db, coll).getFullName()
            });
            return coordinatorDoc !== null && coordinatorDoc.cloneTimestamp !== undefined;
        });

        // Change bucket2's _id to match bucket1's _id.
        const newId = getTimeseriesCollForRawOps(db, coll).findOneWithRawData({'meta.x': -2})._id;
        const replacementBucket =
            getTimeseriesCollForRawOps(db, coll).findOneWithRawData({'meta.x': 2});
        const oldId = replacementBucket._id;
        replacementBucket._id = newId;
        getTimeseriesCollForRawOps(db, coll).remove({_id: oldId}, getRawOperationSpec(db));
        getTimeseriesCollForRawOps(db, coll).insert(replacementBucket, getRawOperationSpec(db));
    },
    {expectedErrorCode: 5356800});

reshardingTest.teardown();
