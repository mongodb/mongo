/*
 * Test that unsplittable collections can be sharded.
 * @tags: [
 *   # Needed to run createUnsplittableCollection
 *   featureFlagAuthoritativeShardCollection,
 *   assumes_balancer_off,
 * ]
 */

import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {findChunksUtil} from "jstests/sharding/libs/find_chunks_util.js";

const kDbName = "test";

const st = new ShardingTest({shards: 2});

const collPrefix = "foo";
let collCounter = 0;

function getNewCollName() {
    let newCollName = collPrefix + collCounter;
    collCounter++;
    return newCollName;
}

function checkCollAndChunks(collName, bucketsNs, unsplittable, shardKey, numChunks) {
    let configDb = st.s.getDB('config');
    let nss = bucketsNs ? kDbName + ".system.buckets." + collName : kDbName + "." + collName;

    let coll = configDb.collections.findOne({_id: nss});
    assert.eq(coll._id, nss);
    if (unsplittable || coll.unsplittable) {
        assert.eq(coll.unsplittable, unsplittable);
    }
    assert.eq(coll.key, shardKey);

    assert.eq(findChunksUtil.countChunksForNs(configDb, nss), numChunks);
}

function checkCRUDWorks(collName, doc) {
    let coll = st.s.getDB(kDbName).getCollection(collName);

    assert.commandWorked(coll.insert(doc));
    assert.eq(coll.countDocuments(doc), 1);
    assert.commandWorked(coll.remove(doc));
}

function checkIndexes(collName, expectedIndexCount) {
    let coll = st.s.getDB(kDbName).getCollection(collName);

    assert.eq(coll.getIndexes().length, expectedIndexCount);
}

function checkChunkLocation(collName, location) {
    let configDb = st.s.getDB('config');
    let nss = kDbName + "." + collName;

    let coll = configDb.collections.findOne({_id: nss});
    let configChunks = configDb.chunks.find({uuid: coll.uuid}).toArray();

    assert.eq(configChunks[0].shard, location);
}

function runTests(dataShard) {
    jsTest.log("Sharded --> Unsplittable OK");
    {
        const kColl = getNewCollName();
        const kNss = kDbName + "." + kColl;

        assert.commandWorked(
            st.s.getDB(kDbName).adminCommand({shardCollection: kNss, key: {_id: 1}}));

        checkCollAndChunks(kColl, false, false, {_id: 1}, 1);
        checkCRUDWorks(kColl, {_id: 1});

        assert.commandWorked(st.s.getDB(kDbName).runCommand({createUnsplittableCollection: kColl}));

        checkCollAndChunks(kColl, false, false, {_id: 1}, 1);
        checkCRUDWorks(kColl, {_id: 1});
    }

    jsTest.log("Unsplittable --> Range based shard key, _id");
    {
        const kColl = getNewCollName();
        const kNss = kDbName + "." + kColl;

        assert.commandWorked(st.s.getDB(kDbName).runCommand(
            {createUnsplittableCollection: kColl, dataShard: dataShard}));

        checkCollAndChunks(kColl, false, true, {_id: 1}, 1);
        checkChunkLocation(kColl, dataShard);
        checkCRUDWorks(kColl, {_id: 1});
        checkIndexes(kColl, 1);

        assert.commandWorked(
            st.s.getDB(kDbName).adminCommand({shardCollection: kNss, key: {_id: 1}}));

        checkCollAndChunks(kColl, false, false, {_id: 1}, 1);
        checkChunkLocation(kColl, dataShard);
        checkCRUDWorks(kColl, {_id: 1});
        checkIndexes(kColl, 1);
    }

    jsTest.log("Unsplittable --> Range based shard key, x");
    {
        const kColl = getNewCollName();
        const kNss = kDbName + "." + kColl;

        assert.commandWorked(st.s.getDB(kDbName).runCommand(
            {createUnsplittableCollection: kColl, dataShard: dataShard}));

        checkCollAndChunks(kColl, false, true, {_id: 1}, 1);
        checkChunkLocation(kColl, dataShard);
        checkCRUDWorks(kColl, {_id: 1});
        checkIndexes(kColl, 1);

        assert.commandWorked(
            st.s.getDB(kDbName).adminCommand({shardCollection: kNss, key: {x: 1}}));

        checkCollAndChunks(kColl, false, false, {x: 1}, 1);
        checkChunkLocation(kColl, dataShard);
        checkCRUDWorks(kColl, {x: 1});
        checkIndexes(kColl, 2);
    }

    jsTest.log("Unsplittable --> Range based shard key, x, non-empty collection");
    {
        const kColl = getNewCollName();
        const kNss = kDbName + "." + kColl;

        assert.commandWorked(st.s.getDB(kDbName).runCommand(
            {createUnsplittableCollection: kColl, dataShard: dataShard}));

        checkCollAndChunks(kColl, false, true, {_id: 1}, 1);
        checkChunkLocation(kColl, dataShard);
        checkCRUDWorks(kColl, {_id: 1});
        checkIndexes(kColl, 1);

        assert.commandWorked(st.s.getDB(kDbName).getCollection(kColl).insertOne({x: 10}));

        assert.commandFailedWithCode(st.s.adminCommand({shardCollection: kNss, key: {x: 1}}),
                                     ErrorCodes.InvalidOptions);

        assert.commandWorked(st.s.getDB(kDbName).getCollection(kColl).createIndex({x: 1}));

        assert.commandWorked(
            st.s.getDB(kDbName).adminCommand({shardCollection: kNss, key: {x: 1}}));

        checkCollAndChunks(kColl, false, false, {x: 1}, 1);
        checkChunkLocation(kColl, dataShard);
        checkCRUDWorks(kColl, {x: 1});
        checkIndexes(kColl, 2);
    }

    jsTest.log("Unsplittable --> Range based shard key, x, with multi-key index");
    {
        const kColl = getNewCollName();
        const kNss = kDbName + "." + kColl;

        assert.commandWorked(st.s.getDB(kDbName).runCommand(
            {createUnsplittableCollection: kColl, dataShard: dataShard}));

        checkCollAndChunks(kColl, false, true, {_id: 1}, 1);
        checkChunkLocation(kColl, dataShard);
        checkCRUDWorks(kColl, {_id: 1});
        checkIndexes(kColl, 1);

        assert.commandWorked(st.s.getDB(kDbName).getCollection(kColl).createIndex({x: 1}));
        assert.commandWorked(st.s.getDB(kDbName).getCollection(kColl).insertOne({x: [1, 3]}));

        assert.commandFailedWithCode(st.s.adminCommand({shardCollection: kNss, key: {x: 1}}),
                                     ErrorCodes.InvalidOptions);

        st.s.getDB(kDbName).getCollection(kColl).remove({x: [1, 3]});

        assert.commandFailedWithCode(st.s.adminCommand({shardCollection: kNss, key: {x: 1}}),
                                     ErrorCodes.InvalidOptions);

        checkCollAndChunks(kColl, false, true, {_id: 1}, 1);
        checkChunkLocation(kColl, dataShard);
        checkCRUDWorks(kColl, {_id: 1});
        checkIndexes(kColl, 2);
    }

    jsTest.log("Unsplittable --> Range based shard key, x, with different options");
    {
        const kColl = getNewCollName();
        const kNss = kDbName + "." + kColl;

        assert.commandWorked(st.s.getDB(kDbName).runCommand(
            {createUnsplittableCollection: kColl, dataShard: dataShard}));

        checkCollAndChunks(kColl, false, true, {_id: 1}, 1);
        checkChunkLocation(kColl, dataShard);
        checkCRUDWorks(kColl, {_id: 1});
        checkIndexes(kColl, 1);

        assert.commandWorked(
            st.s.getDB(kDbName).adminCommand({shardCollection: kNss, key: {x: 1}, unique: true}));

        checkCollAndChunks(kColl, false, false, {x: 1}, 1);
        checkChunkLocation(kColl, dataShard);
        checkCRUDWorks(kColl, {x: 1});
        checkIndexes(kColl, 2);
    }

    let expectedNumChunks = 2;
    // TODO SERVER-81884: update once 8.0 becomes last LTS
    if (!FeatureFlagUtil.isPresentAndEnabled(st.s.getDB(kDbName),
                                             "OneChunkPerShardEmptyCollectionWithHashedShardKey")) {
        expectedNumChunks = 4;
    }

    jsTest.log("Unsplittable --> Hashed shard key, _id");
    {
        const kColl = getNewCollName();
        const kNss = kDbName + "." + kColl;

        assert.commandWorked(st.s.getDB(kDbName).runCommand(
            {createUnsplittableCollection: kColl, dataShard: dataShard}));

        checkCollAndChunks(kColl, false, true, {_id: 1}, 1);
        checkCRUDWorks(kColl, {_id: 1});
        checkIndexes(kColl, 1);

        assert.commandWorked(
            st.s.getDB(kDbName).adminCommand({shardCollection: kNss, key: {_id: "hashed"}}));

        checkCollAndChunks(kColl, false, false, {_id: "hashed"}, expectedNumChunks);
        checkCRUDWorks(kColl, {_id: 1});
        checkIndexes(kColl, 2);
    }

    jsTest.log("Unsplittable --> Hashed shard key, x");
    {
        const kColl = getNewCollName();
        const kNss = kDbName + "." + kColl;

        assert.commandWorked(st.s.getDB(kDbName).runCommand(
            {createUnsplittableCollection: kColl, dataShard: dataShard}));

        checkCollAndChunks(kColl, false, true, {_id: 1}, 1);
        checkCRUDWorks(kColl, {_id: 1});
        checkIndexes(kColl, 1);

        assert.commandWorked(
            st.s.getDB(kDbName).adminCommand({shardCollection: kNss, key: {x: "hashed"}}));

        checkCollAndChunks(kColl, false, false, {x: "hashed"}, expectedNumChunks);
        checkCRUDWorks(kColl, {x: 1});
        checkIndexes(kColl, 2);
    }

    jsTest.log("Unsplittable --> Hashed shard key, x, with additional indexes");
    {
        const kColl = getNewCollName();
        const kNss = kDbName + "." + kColl;

        assert.commandWorked(st.s.getDB(kDbName).runCommand(
            {createUnsplittableCollection: kColl, dataShard: dataShard}));

        checkCollAndChunks(kColl, false, true, {_id: 1}, 1);
        checkCRUDWorks(kColl, {_id: 1});
        checkIndexes(kColl, 1);

        assert.commandWorked(
            st.s.getDB(kDbName).getCollection(kColl).createIndexes([{y: 1}, {z: 1}]));

        checkCollAndChunks(kColl, false, true, {_id: 1}, 1);
        checkCRUDWorks(kColl, {_id: 1});
        checkIndexes(kColl, 3);

        assert.commandWorked(
            st.s.getDB(kDbName).adminCommand({shardCollection: kNss, key: {x: "hashed"}}));

        checkCollAndChunks(kColl, false, false, {x: "hashed"}, expectedNumChunks);
        checkCRUDWorks(kColl, {x: 1});
        checkIndexes(kColl, 4);
    }

    jsTest.log("Unsplittable timeseries without meta field");
    {
        const kColl = getNewCollName();
        const kNss = kDbName + "." + kColl;

        assert.commandWorked(st.s.getDB(kDbName).runCommand({
            createUnsplittableCollection: kColl,
            dataShard: dataShard,
            timeseries: {timeField: 'time'}
        }));

        checkCollAndChunks(kColl, true, true, {_id: 1}, 1);
        checkCRUDWorks(kColl, {time: ISODate("2021-05-18T08:00:00.000Z")});
        checkIndexes(kColl, 0);

        assert.commandWorked(
            st.s.getDB(kDbName).adminCommand({shardCollection: kNss, key: {time: 1}}));

        checkCollAndChunks(kColl, true, false, {"control.min.time": 1}, 1);
        checkCRUDWorks(kColl, {time: ISODate("2021-05-18T08:00:00.000Z")});
        checkIndexes(kColl, 1);
    }

    jsTest.log("Unsplittable timeseries with meta field");
    {
        const kColl = getNewCollName();
        const kNss = kDbName + "." + kColl;

        assert.commandWorked(st.s.getDB(kDbName).runCommand({
            createUnsplittableCollection: kColl,
            dataShard: dataShard,
            timeseries: {timeField: 'time', metaField: 'tag'}
        }));

        checkCollAndChunks(kColl, true, true, {_id: 1}, 1);
        checkCRUDWorks(kColl, {time: ISODate("2021-05-18T08:00:00.000Z")});
        checkIndexes(kColl, 1);

        assert.commandWorked(
            st.s.getDB(kDbName).adminCommand({shardCollection: kNss, key: {'tag.subField': 1}}));

        checkCollAndChunks(kColl, true, false, {"meta.subField": 1}, 1);
        checkCRUDWorks(kColl, {time: ISODate("2021-05-18T08:00:00.000Z")});
        checkIndexes(kColl, 2);
    }
}

jsTest.log("Run tests where dataShard = " + st.shard0.shardName);
runTests(st.shard0.shardName);

jsTest.log("Run tests where dataShard = " + st.shard1.shardName);
runTests(st.shard1.shardName);

st.stop();
