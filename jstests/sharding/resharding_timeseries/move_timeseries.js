// Basic tests for resharding for timeseries collection.
// @tags: [
//   featureFlagMoveCollection,
//   multiversion_incompatible,
//   assumes_balancer_off,
//   requires_fcv_80,
// ]
//

import {ReshardingTest} from "jstests/sharding/libs/resharding_test_fixture.js";

const ns = "reshardingDb.coll";
const bucketNss = "reshardingDb.system.buckets.coll";

const reshardingTest = new ReshardingTest({numDonors: 2});
reshardingTest.setup();
const st = reshardingTest._st;

const timeseriesInfo = {
    timeField: 'ts',
    metaField: 'mFld'
};

const timeseriesCollection = reshardingTest.createUnshardedCollection({
    ns: ns,
    primaryShardName: st.shard0.shardName,
    collOptions: {
        timeseries: timeseriesInfo,
        collation: {locale: "en_US", strength: 2},
    }
});

const metaIndexName = "meta_x_1";
assert.commandWorked(timeseriesCollection.createIndex(
    {'mFld.x': 1}, {name: metaIndexName, collation: {locale: "simple"}}));
const preReshardingMetaIndexSpec =
    timeseriesCollection.getIndexes().filter(idx => idx.name === metaIndexName);

const measurementIndexName = "a_1";
assert.commandWorked(timeseriesCollection.createIndex(
    {a: 1}, {name: measurementIndexName, collation: {locale: "simple"}}));
const preReshardingMeasurementIndexSpec =
    timeseriesCollection.getIndexes().filter(idx => idx.name === measurementIndexName);

// Insert some docs
assert.commandWorked(timeseriesCollection.insert([
    {data: 1, ts: new Date(), mFld: {x: 1, y: -1}},
    {data: 6, ts: new Date(), mFld: {x: 1, y: -1}},
    {data: 3, ts: new Date(), mFld: {x: 2, y: -2}},
    {data: -3, ts: new Date(), mFld: {x: 2, y: -2}},
    {data: 3, ts: new Date(), mFld: {x: 4, y: -3}},
    {data: 1, ts: new Date(), mFld: {x: 5, y: -4}}
]));

assert.eq(4, st.s0.getCollection(bucketNss).countDocuments({}));
assert.eq(6, st.s0.getCollection(ns).countDocuments({}));

reshardingTest.withMoveCollectionInBackground({toShard: st.shard2.shardName}, () => {
    reshardingTest.awaitCloneTimestampChosen();
    assert.commandWorked(timeseriesCollection.insert([
        {data: -6, ts: new Date(), mFld: {x: 1, y: -1}},
        {data: -6, ts: new Date(), mFld: {x: 8, y: 8}},
    ]));
});

let timeseriesCollDocPostResharding = st.config.collections.findOne({_id: bucketNss});
// Resharding keeps timeseries fields.
assert.eq(timeseriesCollDocPostResharding.timeseriesFields.timeField, timeseriesInfo.timeField);
assert.eq(timeseriesCollDocPostResharding.timeseriesFields.metaField, timeseriesInfo.metaField);
// Resharding has updated shard key.
assert.eq(timeseriesCollDocPostResharding.key, {"_id": 1});
assert.eq(timeseriesCollDocPostResharding.unsplittable, true);

assert.eq(5, st.rs2.getPrimary().getCollection(bucketNss).countDocuments({}));
assert.eq(0, st.rs0.getPrimary().getCollection(bucketNss).countDocuments({}));
assert.eq(8, st.s0.getCollection(ns).countDocuments({}));

const postReshardingMetaIndexSpec =
    timeseriesCollection.getIndexes().filter(idx => idx.name === metaIndexName);
assert.eq(preReshardingMetaIndexSpec, postReshardingMetaIndexSpec);

const postReshardingMeasurementIndexSpec =
    timeseriesCollection.getIndexes().filter(idx => idx.name === measurementIndexName);
assert.eq(preReshardingMeasurementIndexSpec, postReshardingMeasurementIndexSpec);

reshardingTest.teardown();
