/*
 * Test that unsplittable collections can be sharded.
 * @tags: [
 *   featureFlagTrackUnshardedCollectionsOnShardingCatalog,
 *   multiversion_incompatible,
 *   assumes_balancer_off,
 * ]
 */

import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";

const kDbName = "test";

const st = new ShardingTest({shards: 2});

const collPrefix = "foo";
let collCounter = 0;

assert.commandWorked(st.s.getDB("admin").runCommand({enableSharding: kDbName}));

function getNewCollName() {
    let newCollName = collPrefix + collCounter;
    collCounter++;
    return newCollName;
}

function checkCollAndChunks(collName, unsplittable, shardKey, numChunks) {
    let configDb = st.s.getDB('config');
    let nss = kDbName + "." + collName;

    let coll = configDb.collections.findOne({_id: nss});
    assert.eq(coll._id, nss);
    if (unsplittable || coll.unsplittable) {
        assert.eq(coll.unsplittable, unsplittable);
    }
    assert.eq(coll.key, shardKey);

    let configChunks = configDb.chunks.find({uuid: coll.uuid}).toArray();
    assert.eq(configChunks.length, numChunks);
}

jsTest.log("Sharded --> Unsplittable fails");
{
    const kColl = getNewCollName();
    const kNss = kDbName + "." + kColl;

    assert.commandWorked(st.s.getDB(kDbName).adminCommand({shardCollection: kNss, key: {_id: 1}}));

    checkCollAndChunks(kColl, false, {_id: 1}, 1);

    assert.commandFailedWithCode(
        st.s.getDB(kDbName).runCommand({createUnsplittableCollection: kColl}),
        ErrorCodes.AlreadyInitialized);

    checkCollAndChunks(kColl, false, {_id: 1}, 1);
}

jsTest.log("Unsplittable --> Range based shard key, _id");
{
    const kColl = getNewCollName();
    const kNss = kDbName + "." + kColl;

    assert.commandWorked(st.s.getDB(kDbName).runCommand({createUnsplittableCollection: kColl}));

    checkCollAndChunks(kColl, true, {_id: 1}, 1);

    assert.commandWorked(st.s.getDB(kDbName).adminCommand({shardCollection: kNss, key: {_id: 1}}));

    checkCollAndChunks(kColl, false, {_id: 1}, 1);
}

jsTest.log("Unsplittable --> Range based shard key, x");
{
    const kColl = getNewCollName();
    const kNss = kDbName + "." + kColl;

    assert.commandWorked(st.s.getDB(kDbName).runCommand({createUnsplittableCollection: kColl}));

    checkCollAndChunks(kColl, true, {_id: 1}, 1);

    assert.commandWorked(st.s.getDB(kDbName).adminCommand({shardCollection: kNss, key: {x: 1}}));

    checkCollAndChunks(kColl, false, {x: 1}, 1);
}

jsTest.log("Unsplittable --> Range based shard key, x, with different options");
{
    const kColl = getNewCollName();
    const kNss = kDbName + "." + kColl;

    assert.commandWorked(st.s.getDB(kDbName).runCommand({createUnsplittableCollection: kColl}));

    checkCollAndChunks(kColl, true, {_id: 1}, 1);

    assert.commandWorked(
        st.s.getDB(kDbName).adminCommand({shardCollection: kNss, key: {x: 1}, unique: true}));

    checkCollAndChunks(kColl, false, {x: 1}, 1);
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

    assert.commandWorked(st.s.getDB(kDbName).runCommand({createUnsplittableCollection: kColl}));

    checkCollAndChunks(kColl, true, {_id: 1}, 1);

    assert.commandWorked(
        st.s.getDB(kDbName).adminCommand({shardCollection: kNss, key: {_id: "hashed"}}));

    checkCollAndChunks(kColl, false, {_id: "hashed"}, expectedNumChunks);
}

jsTest.log("Unsplittable --> Hashed shard key, x");
{
    const kColl = getNewCollName();
    const kNss = kDbName + "." + kColl;

    assert.commandWorked(st.s.getDB(kDbName).runCommand({createUnsplittableCollection: kColl}));

    checkCollAndChunks(kColl, true, {_id: 1}, 1);

    assert.commandWorked(
        st.s.getDB(kDbName).adminCommand({shardCollection: kNss, key: {x: "hashed"}}));

    checkCollAndChunks(kColl, false, {x: "hashed"}, expectedNumChunks);
}

st.stop();
