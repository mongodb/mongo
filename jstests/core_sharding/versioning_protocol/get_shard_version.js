/*
 * Test covering the consistency of the information returned by getShardVersion against other
 * commands triggering placement changes of the targeted namespaces.
 * @tags: [
 *   # Implicit shardCollection invocations alter the values of the collected stats.
 *   assumes_unsharded_collection,
 *   # The test performs migrations and requires strict control over each collection placement.
 *   requires_2_or_more_shards,
 *   assumes_balancer_off,
 *   assumes_no_track_upon_creation,
 *   # Commands based on reshardCollection require a custom value for the
 *   # minSnapshotHistoryWindowInSeconds server parameter, which is reset when a node is killed.
 *   does_not_support_stepdowns,
 * ]
 */

import {getRandomShardName} from "jstests/libs/sharded_cluster_fixture_helpers.js";

assert.commandWorked(db.dropDatabase());

const kDbName = db.getName();
const kCollName = 'testColl';
const coll = db.getCollection(kCollName);
const kNss = coll.getFullName();

function getShardVersionResponse(nss, failureExpected = false) {
    const responseFields = ['version', 'versionEpoch', 'versionTimestamp'];
    // Ensure that the command retrieves up-to-date cached information.
    assert.commandWorked(db.adminCommand({flushRouterConfig: nss}));
    const response = db.adminCommand({getShardVersion: nss});
    if (failureExpected) {
        assert.commandFailedWithCode(response, ErrorCodes.NamespaceNotFound);
        return null;
    }

    assert.commandWorked(response);
    responseFields.forEach(
        field =>
            assert(response[field],
                   `Missing  or null field ${field} in shardVersion response ${tojson(response)}`));
    return response;
}

function assertMinorVersionIncrease(before, after) {
    assert.eq(before.version.getTime(), after.version.getTime());
    assert.lt(before.version.getInc(), after.version.getInc());
    assert.eq(before.versionEpoch, after.versionEpoch);
    assert.eq(before.versionTimestamp, after.versionTimestamp);
}

function assertMajorVersionIncrease(before, after) {
    assert.lt(before.version.getTime(), after.version.getTime());
    assert.eq(1, after.version.getInc());
    assert.eq(before.versionEpoch, after.versionEpoch);
    assert.eq(before.versionTimestamp, after.versionTimestamp);
}

function assertEpochChange(before, after) {
    assert.neq(before.versionEpoch, after.versionEpoch);
    assert.eq(-1, timestampCmp(before.versionTimestamp, after.versionTimestamp));
}

jsTest.log('getShardVersion fails when the parent db does not exist');
getShardVersionResponse(kNss, true /*failureExpected*/);

jsTest.log('getShardVersion fails when run against non-yet created collection name');
assert.commandWorked(db.adminCommand({enableSharding: kDbName}));
const primaryShard = db.getDatabasePrimaryShardId();
const anotherShard = getRandomShardName(db, /* exclude =*/[primaryShard]);
getShardVersionResponse(kNss, true /*failureExpected*/);

jsTest.log('getShardVersion fails when run against an unsharded collection name');
assert.commandWorked(coll.createIndex({x: 1}));
getShardVersionResponse(kNss, true /*failureExpected*/);

jsTest.log('getShardVersion succeeds when run against a sharded collection name');
assert.commandWorked(db.adminCommand({shardCollection: kNss, key: {x: 1}}));
const atCollectionShardedTime = getShardVersionResponse(kNss);

jsTest.log('getShardVersion returns an updated version upon a chunk split');
assert.commandWorked(db.adminCommand({split: kNss, middle: {x: 0}}));
const atChunkSplitTime = getShardVersionResponse(kNss);
assertMinorVersionIncrease(atCollectionShardedTime, atChunkSplitTime);

jsTest.log('getShardVersion returns an updated version upon a chunk merge');
assert.commandWorked(db.adminCommand({split: kNss, middle: {x: 10}}));
const beforeChunkMerge = getShardVersionResponse(kNss);
assert.commandWorked(db.adminCommand({mergeChunks: kNss, bounds: [{x: 0}, {x: MaxKey}]}));
const afterChunkMerge = getShardVersionResponse(kNss);
assertMinorVersionIncrease(beforeChunkMerge, afterChunkMerge);

jsTest.log('getShardVersion returns an updated version upon a chunk migration');
assert.commandWorked(db.adminCommand({movechunk: kNss, find: {x: 1}, to: anotherShard}));
const atChunkMigratedTime = getShardVersionResponse(kNss);
assertMajorVersionIncrease(afterChunkMerge, atChunkMigratedTime);

jsTest.log(
    ' getShardVersion returns an updated version upon dropping and recreating a sharded collection');
assert.commandWorked(db.runCommand({drop: kCollName}));
getShardVersionResponse(kNss, true /*failureExpected*/);
assert.commandWorked(db.adminCommand({shardCollection: kNss, key: {x: 1}}));
const atCollectionRecreatedTime = getShardVersionResponse(kNss);
assertEpochChange(atChunkMigratedTime, atCollectionRecreatedTime);

jsTest.log('getShardVersion returns an updated version upon refining a shard key');
assert.commandWorked(coll.createIndex({x: 1, y: 1}));
assert.commandWorked(db.adminCommand({refineCollectionShardKey: kNss, key: {x: 1, y: 1}}));
const atShardKeyRefinedTime = getShardVersionResponse(kNss);
assertEpochChange(atCollectionRecreatedTime, atShardKeyRefinedTime);

jsTest.log('getShardVersion returns an updated version upon a resharding operation');
assert.commandWorked(db.adminCommand({reshardCollection: kNss, key: {y: 1}, numInitialChunks: 1}));
const atCollectionReshardedTime = getShardVersionResponse(kNss);
assertEpochChange(atShardKeyRefinedTime, atCollectionReshardedTime);

jsTest.log('getShardVersion returns information on an unsplittable collection');
const kUnsplittableCollName = 'unsplittableColl';
const kUnsplittableNss = db[kUnsplittableCollName].getFullName();
assert.commandWorked(db[kUnsplittableCollName].insert({x: 1}));
assert.commandWorked(db.adminCommand({moveCollection: kUnsplittableNss, toShard: anotherShard}));
getShardVersionResponse(kUnsplittableNss);

jsTest.log('getShardVersion fails upon untracking an unsplittable collection');
assert.commandWorked(db.adminCommand({moveCollection: kUnsplittableNss, toShard: primaryShard}));
assert.commandWorked(db.adminCommand({untrackUnshardedCollection: kUnsplittableNss}));
getShardVersionResponse(kUnsplittableNss, true /*failureExpected*/);
