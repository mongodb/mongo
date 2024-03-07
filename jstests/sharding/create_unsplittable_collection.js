/*
 * Test the test command createUnsplittableCollection. This command is a temporary wrapper on
 * shardCollection that allows you to create unsplittable collection aka tracked unsharded
 * collection. Since we use the same coordinator, we both check the createUnsplittableCollection
 * works and that shardCollection won't generate unsplittable collection.
 * @tags: [
 *   featureFlagTrackUnshardedCollectionsUponCreation,
 *   multiversion_incompatible,
 *   assumes_balancer_off,
 * ]
 */

const kDbName = "test";
const kTimeseriesColl = 'timeseriesColl';
const kTimeseriesColl2 = 'timeseriesColl2';
const kTimeseriesColl3 = 'timeseriesColl3';
const kNss = kDbName + '.' + kTimeseriesColl;
const kNss2 = kDbName + '.' + kTimeseriesColl2;
const kNss3 = kDbName + '.' + kTimeseriesColl3;
const kBucketNss = kDbName + '.system.buckets.' + kTimeseriesColl;
const kBucketNss2 = kDbName + '.system.buckets.' + kTimeseriesColl2;
const kBucketNss3 = kDbName + '.system.buckets.' + kTimeseriesColl3;

const st = new ShardingTest({shards: 2});
const mongos = st.s;
const shard0 = st.shard0.shardName;
const shard1 = st.shard1.shardName;

// Ensure the db primary is shard0. This will be expected later on.
st.s.adminCommand({enableSharding: kDbName, primaryShard: shard0});

jsTest.log("Running test command createUnsplittableCollection to track an unsharded collection");
{
    const kColl = "first_unsharded_collection";
    const kNssUnsharded = kDbName + "." + kColl;
    assert.commandWorked(mongos.getDB("admin").runCommand({enableSharding: kDbName}));

    let result = st.s.getDB(kDbName).runCommand({createUnsplittableCollection: kColl});
    assert.commandWorked(result);

    // checking consistency
    let configDb = mongos.getDB('config');

    let unshardedColl = configDb.collections.findOne({_id: kNssUnsharded});
    assert.eq(unshardedColl._id, kNssUnsharded);
    assert.eq(unshardedColl._id, kNssUnsharded);
    assert.eq(unshardedColl.unsplittable, true);
    assert.eq(unshardedColl.key, {_id: 1});

    let configChunks = configDb.chunks.find({uuid: unshardedColl.uuid}).toArray();
    assert.eq(configChunks.length, 1);
}

jsTest.log('Check that createCollection can create a tracked unsharded collection');
{
    const kDataColl = 'unsplittable_collection_on_create_collection';
    const kDataCollNss = kDbName + '.' + kDataColl;

    assert.commandWorked(st.s.getDB(kDbName).createCollection(kDataColl));

    // running the same request again will behave as no-op
    assert.commandWorked(st.s.getDB(kDbName).createCollection(kDataColl));

    let res = assert.commandWorked(
        st.getPrimaryShard(kDbName).getDB(kDbName).runCommand({listIndexes: kDataColl}));
    let indexes = res.cursor.firstBatch;
    assert(indexes.length === 1);

    let col = st.s.getCollection('config.collections').findOne({_id: kDataCollNss});
    assert.eq(col.unsplittable, true);
    assert.eq(col.key, {_id: 1});
    assert.eq(st.s.getCollection('config.chunks').countDocuments({uuid: col.uuid}), 1);
}

jsTest.log('When "capped" is true, the "size" field needs to be present.');
{
    const kDataColl = 'unsplittable_collection_on_create_collection_capped_size';

    // Creating a collection that already exists with different options reports failure.
    assert.commandFailedWithCode(st.s.getDB(kDbName).createCollection(kDataColl, {capped: true}),
                                 ErrorCodes.InvalidOptions);

    assert.commandFailedWithCode(
        st.s.getDB(kDbName).createCollection(kDataColl, {capped: true, max: 10}),
        ErrorCodes.InvalidOptions);
}

jsTest.log('If a view already exists with same namespace fail with NamespaceExists');
{
    const kDataColl = 'simple_view';

    assert.commandWorked(st.s.getDB(kDbName).createView(kDataColl, kDbName + '.simple_coll', []));

    assert.commandFailedWithCode(st.s.getDB(kDbName).createCollection(kDataColl),
                                 [ErrorCodes.NamespaceExists]);
}

jsTest.log('Check that shardCollection won\'t generate an unsplittable collection');
{
    const kCollSharded = 'sharded_collection';
    const kNssSharded = kDbName + '.' + kCollSharded;

    let result = mongos.adminCommand({shardCollection: kNssSharded, key: {_id: 1}});
    assert.commandWorked(result);

    let shardedColl = mongos.getDB('config').collections.findOne({_id: kNssSharded});
    assert.eq(shardedColl.unsplittable, undefined);
}

jsTest.log('Running command to create a timeseries collection');
{
    assert.commandWorked(st.s.getDB(kDbName).runCommand(
        {createUnsplittableCollection: kTimeseriesColl, timeseries: {timeField: 'time'}}));
    const collMetadata = st.s.getCollection('config.collections').findOne({_id: kBucketNss});
    assert.eq(1, st.s.getCollection('config.chunks').countDocuments({uuid: collMetadata.uuid}));
}

jsTest.log('Create a timeseries collection with a meta field');
{
    assert.commandWorked(st.s.getDB(kDbName).runCommand({
        createUnsplittableCollection: kTimeseriesColl2,
        timeseries: {timeField: 'time', metaField: 'tag'}
    }));
    const collMetadata = st.s.getCollection('config.collections').findOne({_id: kBucketNss2});
    assert.eq(1, st.s.getCollection('config.chunks').countDocuments({uuid: collMetadata.uuid}));
}

jsTest.log('Shard an unexistent timeseries collection');
{
    assert.commandWorked(st.s.adminCommand(
        {shardCollection: kNss3, key: {time: 1}, timeseries: {timeField: 'time'}}));
    const collMetadata = st.s.getCollection('config.collections').findOne({_id: kBucketNss3});
    assert.eq(1, st.s.getCollection('config.chunks').countDocuments({uuid: collMetadata.uuid}));
}

jsTest.log('Shard an unsplittable timeseries collection');
{
    assert.commandWorked(st.s.adminCommand(
        {shardCollection: kNss, key: {time: 1}, timeseries: {timeField: 'time'}}));
    const collMetadata = st.s.getCollection('config.collections').findOne({_id: kBucketNss});
    assert.eq(1, st.s.getCollection('config.chunks').countDocuments({uuid: collMetadata.uuid}));
}

st.stop();
