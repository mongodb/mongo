// Basic tests for resharding for timeseries collection.
// @tags: [
//   featureFlagMoveCollection,
//   multiversion_incompatible,
//   assumes_balancer_off,
//   requires_fcv_80,
// ]
//

import {getTimeseriesCollForDDLOps} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";
import {getRawOperationSpec, getTimeseriesCollForRawOps} from "jstests/libs/raw_operation_utils.js";
import {ReshardingTest} from "jstests/sharding/libs/resharding_test_fixture.js";

const ns = "reshardingDb.coll";

const reshardingTest = new ReshardingTest({numDonors: 2});
reshardingTest.setup();
const st = reshardingTest._st;

const timeseriesInfo = {
    timeField: 'ts',
    metaField: 'mFld'
};

const coll = reshardingTest.createUnshardedCollection({
    ns: ns,
    primaryShardName: st.shard0.shardName,
    collOptions: {
        timeseries: timeseriesInfo,
        collation: {locale: "en_US", strength: 2},
    }
});
const db = coll.getDB();

const metaIndexName = "meta_x_1";
assert.commandWorked(
    coll.createIndex({'mFld.x': 1}, {name: metaIndexName, collation: {locale: "simple"}}));
const preReshardingMetaIndexSpec = coll.getIndexes().filter(idx => idx.name === metaIndexName);

const measurementIndexName = "a_1";
assert.commandWorked(
    coll.createIndex({a: 1}, {name: measurementIndexName, collation: {locale: "simple"}}));
const preReshardingMeasurementIndexSpec =
    coll.getIndexes().filter(idx => idx.name === measurementIndexName);

// Insert some docs
assert.commandWorked(coll.insert([
    {data: 1, ts: new Date(), mFld: {x: 1, y: -1}},
    {data: 6, ts: new Date(), mFld: {x: 1, y: -1}},
    {data: 3, ts: new Date(), mFld: {x: 2, y: -2}},
    {data: -3, ts: new Date(), mFld: {x: 2, y: -2}},
    {data: 3, ts: new Date(), mFld: {x: 4, y: -3}},
    {data: 1, ts: new Date(), mFld: {x: 5, y: -4}}
]));

assert.eq(4, getTimeseriesCollForRawOps(db, coll).countDocuments({}, getRawOperationSpec(db)));
assert.eq(6, coll.countDocuments({}));

reshardingTest.withMoveCollectionInBackground({toShard: st.shard2.shardName}, () => {
    reshardingTest.awaitCloneTimestampChosen();
    assert.commandWorked(coll.insert([
        {data: -6, ts: new Date(), mFld: {x: 1, y: -1}},
        {data: -6, ts: new Date(), mFld: {x: 8, y: 8}},
    ]));
});

let timeseriesCollDocPostResharding =
    st.config.collections.findOne({_id: getTimeseriesCollForDDLOps(db, coll).getFullName()});
// Resharding keeps timeseries fields.
assert.eq(timeseriesCollDocPostResharding.timeseriesFields.timeField, timeseriesInfo.timeField);
assert.eq(timeseriesCollDocPostResharding.timeseriesFields.metaField, timeseriesInfo.metaField);
// Resharding has updated shard key.
assert.eq(timeseriesCollDocPostResharding.key, {"_id": 1});
assert.eq(timeseriesCollDocPostResharding.unsplittable, true);

assert.eq(5,
          getTimeseriesCollForRawOps(db, st.rs2.getPrimary().getCollection(coll.getFullName()))
              .countDocuments({}, getRawOperationSpec(db)));
assert.eq(0,
          getTimeseriesCollForRawOps(db, st.rs0.getPrimary().getCollection(coll.getFullName()))
              .countDocuments({}, getRawOperationSpec(db)));
assert.eq(8, coll.countDocuments({}));

const postReshardingMetaIndexSpec = coll.getIndexes().filter(idx => idx.name === metaIndexName);
assert.eq(preReshardingMetaIndexSpec, postReshardingMetaIndexSpec);

const postReshardingMeasurementIndexSpec =
    coll.getIndexes().filter(idx => idx.name === measurementIndexName);
assert.eq(preReshardingMeasurementIndexSpec, postReshardingMeasurementIndexSpec);

reshardingTest.teardown();
