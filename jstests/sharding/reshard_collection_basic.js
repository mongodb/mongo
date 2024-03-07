//
// Basic tests for reshardCollection.
// @tags: [
//   uses_atclustertime,
// ]
//

import {DiscoverTopology} from "jstests/libs/discover_topology.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {ReshardCollectionCmdTest} from "jstests/sharding/libs/reshard_collection_util.js";

const st = new ShardingTest({mongos: 1, shards: 2});
const kDbName = 'db';
const collName = 'foo';
const ns = kDbName + '.' + collName;
const mongos = st.s0;
const kNumInitialDocs = 500;
const reshardCmdTest =
    new ReshardCollectionCmdTest({st, dbName: kDbName, collName, numInitialDocs: kNumInitialDocs});

const criticalSectionTimeoutMS = 24 * 60 * 60 * 1000; /* 1 day */
const topology = DiscoverTopology.findConnectedNodes(mongos);
const coordinator = new Mongo(topology.configsvr.nodes[0]);
assert.commandWorked(coordinator.getDB("admin").adminCommand(
    {setParameter: 1, reshardingCriticalSectionTimeoutMillis: criticalSectionTimeoutMS}));

let presetReshardedChunks =
    [{recipientShardId: st.shard1.shardName, min: {newKey: MinKey}, max: {newKey: MaxKey}}];

/**
 * Fail cases
 */

jsTest.log('Fail if sharding is disabled.');
assert.commandFailedWithCode(mongos.adminCommand({reshardCollection: ns, key: {newKey: 1}}),
                             ErrorCodes.NamespaceNotFound);

assert.commandWorked(mongos.adminCommand({enableSharding: kDbName}));

jsTest.log("Fail if collection is unsharded.");
assert.commandFailedWithCode(mongos.adminCommand({reshardCollection: ns, key: {newKey: 1}}),
                             [ErrorCodes.NamespaceNotSharded, ErrorCodes.NamespaceNotFound]);

assert.commandWorked(mongos.adminCommand({shardCollection: ns, key: {oldKey: 1}}));

jsTest.log("Fail if missing required key.");
assert.commandFailedWithCode(mongos.adminCommand({reshardCollection: ns}),
                             ErrorCodes.IDLFailedToParse);

jsTest.log("Fail if unique is specified and is true.");
assert.commandFailedWithCode(
    mongos.adminCommand({reshardCollection: ns, key: {newKey: 1}, unique: true}),
    ErrorCodes.BadValue);

jsTest.log("Fail if collation is specified and is not {locale: 'simple'}.");
assert.commandFailedWithCode(
    mongos.adminCommand({reshardCollection: ns, key: {newKey: 1}, collation: {locale: 'en_US'}}),
    ErrorCodes.BadValue);

jsTest.log("Fail if both numInitialChunks and _presetReshardedChunks are provided.");
assert.commandFailedWithCode(mongos.adminCommand({
    reshardCollection: ns,
    key: {newKey: 1},
    unique: false,
    collation: {locale: 'simple'},
    numInitialChunks: 2,
    _presetReshardedChunks: presetReshardedChunks
}),
                             ErrorCodes.BadValue);

jsTest.log("Fail if the zone provided is not assigned to a shard.");
const nonExistingZoneName = 'x0';
assert.commandFailedWithCode(mongos.adminCommand({
    reshardCollection: ns,
    key: {newKey: 1},
    unique: false,
    collation: {locale: 'simple'},
    zones: [{zone: nonExistingZoneName, min: {newKey: 5}, max: {newKey: 10}}],
    numInitialChunks: 2,
}),
                             ErrorCodes.CannotCreateChunkDistribution);

jsTest.log("Fail if zone provided is invalid for storage.");
assert.commandFailedWithCode(mongos.adminCommand({
    reshardCollection: ns,
    key: {"_id": "hashed"},
    zones: [{min: {"_id": {"$minKey": 1}}, max: {"_id": {"$maxKey": 1}}, zone: "Namezone"}]
}),
                             ErrorCodes.BadValue);

jsTestLog("Fail if splitting collection into multiple chunks while it is still empty.");
assert.commandFailedWithCode(
    mongos.adminCommand({reshardCollection: ns, key: {b: 1}, numInitialChunks: 2}), 4952606);
assert.commandFailedWithCode(
    st.s.adminCommand({reshardCollection: ns, key: {b: "hashed"}, numInitialChunks: 2}), 4952606);

jsTest.log(
    "Fail if authoritative tags exist in config.tags collection and zones are not provided.");
const existingZoneName = 'x1';
assert.commandWorked(
    st.s.adminCommand({addShardToZone: st.shard1.shardName, zone: existingZoneName}));
assert.commandWorked(st.s.adminCommand(
    {updateZoneKeyRange: ns, min: {oldKey: 0}, max: {oldKey: 5}, zone: existingZoneName}));

assert.commandFailedWithCode(mongos.adminCommand({
    reshardCollection: ns,
    key: {newKey: 1},
    unique: false,
    collation: {locale: 'simple'},
    numInitialChunks: 2,
}),
                             ErrorCodes.BadValue);

// TODO SERVER-77915 remove or adapt this test case since a user-created unsharded collection is
// now always tracked. A temporary db.system.resharding.collection must now exist as unsplittable as
// well to support moveCollection
const isTrackUnshardedEnabled = FeatureFlagUtil.isPresentAndEnabled(
    st.s.getDB('admin'), "TrackUnshardedCollectionsUponCreation");
if (!isTrackUnshardedEnabled) {
    jsTestLog("Fail if attempting insert to an unsharded 'system.resharding.' collection");
    assert.commandFailedWithCode(
        mongos.getDB('test').system.resharding.mycoll.insert({_id: 1, a: 1}),
        ErrorCodes.NamespaceNotSharded);
}

/**
 * Success cases
 */

mongos.getDB(kDbName)[collName].drop();

jsTest.log("Succeed when correct locale is provided.");
reshardCmdTest.assertReshardCollOk(
    {reshardCollection: ns, key: {newKey: 1}, collation: {locale: 'simple'}}, 1);

jsTest.log("Succeed base case.");
reshardCmdTest.assertReshardCollOk({reshardCollection: ns, key: {newKey: 1}}, 1);

jsTest.log("Succeed if unique is specified and is false.");
reshardCmdTest.assertReshardCollOk({reshardCollection: ns, key: {newKey: 1}, unique: false}, 1);

jsTest.log(
    "Succeed if _presetReshardedChunks is provided and test commands are enabled (default).");
reshardCmdTest.assertReshardCollOkWithPreset({reshardCollection: ns, key: {newKey: 1}},
                                             presetReshardedChunks);

presetReshardedChunks = [
    {recipientShardId: st.shard0.shardName, min: {newKey: MinKey}, max: {newKey: 0}},
    {recipientShardId: st.shard1.shardName, min: {newKey: 0}, max: {newKey: MaxKey}}
];

jsTest.log("Succeed if all optional fields and numInitialChunks are provided with correct values.");
reshardCmdTest.assertReshardCollOk({
    reshardCollection: ns,
    key: {newKey: 1},
    unique: false,
    collation: {locale: 'simple'},
    numInitialChunks: 2,
},
                                   2);

jsTest.log(
    "Succeed if all optional fields and _presetReshardedChunks are provided with correct values" +
    " and test commands are enabled (default).");
reshardCmdTest.assertReshardCollOkWithPreset(
    {reshardCollection: ns, key: {newKey: 1}, unique: false, collation: {locale: 'simple'}},
    presetReshardedChunks);

jsTest.log("Succeed if the zone provided is assigned to a shard but not a range for the source" +
           " collection.");
const newZoneName = 'x2';
assert.commandWorked(st.s.adminCommand({addShardToZone: st.shard1.shardName, zone: newZoneName}));
reshardCmdTest.assertReshardCollOk({
    reshardCollection: ns,
    key: {newKey: 1},
    unique: false,
    collation: {locale: 'simple'},
    zones: [{zone: newZoneName, min: {newKey: 5}, max: {newKey: 10}}]
},
                                   3);

jsTest.log("Succeed if resulting chunks all end up in one shard.");
reshardCmdTest.assertReshardCollOk({
    reshardCollection: ns,
    key: {newKey: 1},
    unique: false,
    numInitialChunks: 1,
    collation: {locale: 'simple'},
    zones: [{zone: newZoneName, min: {newKey: MinKey}, max: {newKey: MaxKey}}]
},
                                   1);

jsTest.log("Succeed if zones are empty");
reshardCmdTest.assertReshardCollOk({
    reshardCollection: ns,
    key: {newKey: 1},
    unique: false,
    numInitialChunks: 1,
    collation: {locale: 'simple'},
    zones: []
},
                                   1);

jsTest.log("Succeed if zones are not empty.");
assert.commandWorked(
    mongos.adminCommand({addShardToZone: st.shard1.shardName, zone: existingZoneName}));
assert.commandWorked(st.s.adminCommand(
    {updateZoneKeyRange: ns, min: {oldKey: 0}, max: {oldKey: 5}, zone: existingZoneName}));
reshardCmdTest.assertReshardCollOk({
    reshardCollection: ns,
    key: {oldKey: 1, newKey: 1},
    unique: false,
    collation: {locale: 'simple'},
    zones: [{zone: existingZoneName, min: {oldKey: 0}, max: {oldKey: 5}}]
},
                                   3);

jsTest.log("Succeed with hashed shard key that provides enough cardinality.");
assert.commandWorked(
    mongos.adminCommand({shardCollection: ns, key: {a: "hashed"}, numInitialChunks: 5}));
assert.commandWorked(mongos.getCollection(ns).insert(
    Array.from({length: 10000}, () => ({a: new ObjectId(), b: new ObjectId()}))));
assert.commandWorked(
    st.s.adminCommand({reshardCollection: ns, key: {b: "hashed"}, numInitialChunks: 5}));
mongos.getDB(kDbName)[collName].drop();

st.stop();
