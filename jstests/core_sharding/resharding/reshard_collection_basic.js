/**
 * Basic tests for reshardCollection.
 * @tags: [
 *  uses_atclustertime,
 *  # Stepdown test coverage is already provided by the resharding FSM suites.
 *  does_not_support_stepdowns,
 * ]
 */

import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {ReshardCollectionCmdTest} from "jstests/sharding/libs/reshard_collection_util.js";
import {createChunks, getShardNames} from "jstests/sharding/libs/sharding_util.js";

const shardNames = getShardNames(db);
let presetReshardedChunks =
    [{recipientShardId: shardNames[0], min: {newKey: MinKey}, max: {newKey: MaxKey}}];
const collName = jsTestName();
const dbName = db.getName();
const ns = dbName + '.' + collName;
const mongos = db.getMongo();

const numInitialDocs = 500;

const reshardCmdTest = new ReshardCollectionCmdTest(
    {mongos, dbName, collName, numInitialDocs, skipDirectShardChecks: true});

/**
 * Fail cases
 */
jsTest.log('Fail if sharding is disabled.');
assert.commandFailedWithCode(
    db.adminCommand({reshardCollection: ns, key: {newKey: 1}, numInitialChunks: 1}),
    ErrorCodes.NamespaceNotFound);

assert.commandWorked(db.adminCommand({enableSharding: dbName}));

jsTest.log("Fail if collection is unsharded.");
assert.commandFailedWithCode(
    db.adminCommand({reshardCollection: ns, key: {newKey: 1}, numInitialChunks: 1}),
    [ErrorCodes.NamespaceNotSharded, ErrorCodes.NamespaceNotFound]);

assert.commandWorked(db.adminCommand({shardCollection: ns, key: {oldKey: 1}}));

jsTest.log("Fail if missing required key.");
assert.commandFailedWithCode(db.adminCommand({reshardCollection: ns, numInitialChunks: 1}),
                             ErrorCodes.IDLFailedToParse);

jsTest.log("Fail if unique is specified and is true.");
assert.commandFailedWithCode(
    db.adminCommand({reshardCollection: ns, key: {newKey: 1}, unique: true, numInitialChunks: 1}),
    ErrorCodes.BadValue);

jsTest.log("Fail if collation is specified and is not {locale: 'simple'}.");
assert.commandFailedWithCode(db.adminCommand({
    reshardCollection: ns,
    key: {newKey: 1},
    collation: {locale: 'en_US'},
    numInitialChunks: 1
}),
                             ErrorCodes.BadValue);

jsTest.log("Fail if both numInitialChunks and _presetReshardedChunks are provided.");
assert.commandFailedWithCode(db.adminCommand({
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
assert.commandFailedWithCode(db.adminCommand({
    reshardCollection: ns,
    key: {newKey: 1},
    unique: false,
    collation: {locale: 'simple'},
    zones: [{zone: nonExistingZoneName, min: {newKey: 5}, max: {newKey: 10}}],
    numInitialChunks: 2,
}),
                             ErrorCodes.CannotCreateChunkDistribution);

jsTest.log("Fail if zone provided is invalid for storage.");
assert.commandFailedWithCode(db.adminCommand({
    reshardCollection: ns,
    key: {"_id": "hashed"},
    numInitialChunks: 1,
    zones: [{min: {"_id": {"$minKey": 1}}, max: {"_id": {"$maxKey": 1}}, zone: "Namezone"}]
}),
                             ErrorCodes.BadValue);

jsTestLog("Fail if splitting collection into multiple chunks while it is still empty.");
assert.commandFailedWithCode(
    db.adminCommand({reshardCollection: ns, key: {b: 1}, numInitialChunks: 2}), 4952606);
assert.commandFailedWithCode(
    db.adminCommand({reshardCollection: ns, key: {b: "hashed"}, numInitialChunks: 2}), 4952606);

jsTest.log(
    "Fail if authoritative tags exist in config.tags collection and zones are not provided.");
const existingZoneName = 'x1';
assert.commandWorked(db.adminCommand({addShardToZone: shardNames[0], zone: existingZoneName}));
assert.commandWorked(db.adminCommand(
    {updateZoneKeyRange: ns, min: {oldKey: 0}, max: {oldKey: 5}, zone: existingZoneName}));

assert.commandFailedWithCode(db.adminCommand({
    reshardCollection: ns,
    key: {newKey: 1},
    unique: false,
    collation: {locale: 'simple'},
    numInitialChunks: 2,
}),
                             ErrorCodes.BadValue);

// TODO SERVER-87189 remove this test case since a user-created unsharded collection is now always
// tracked. A temporary db.system.resharding.collection must now exist as unsplittable as well
// to support moveCollection.
if (!TestData.implicitlyTrackUnshardedCollectionOnCreation) {
    jsTestLog("Fail if attempting insert to an unsharded 'system.resharding.' collection");
    assert.commandFailedWithCode(
        db.getSiblingDB('test').system.resharding.mycoll.insert({_id: 1, a: 1}),
        [ErrorCodes.NamespaceNotFound, ErrorCodes.NamespaceNotSharded]);
}

/**
 * Success cases
 */

db.getCollection(collName).drop();

jsTest.log("Succeed when correct locale is provided.");
reshardCmdTest.assertReshardCollOk(
    {reshardCollection: ns, key: {newKey: 1}, collation: {locale: 'simple'}, numInitialChunks: 1},
    1);

jsTest.log("Succeed base case.");
reshardCmdTest.assertReshardCollOk({reshardCollection: ns, key: {newKey: 1}, numInitialChunks: 1},
                                   1);

jsTest.log("Succeed with compound shard key.");
reshardCmdTest.assertReshardCollOk(
    {reshardCollection: ns, key: {newKey: 1, oldKey: 1}, numInitialChunks: 2}, 2);

jsTest.log("Succeed if unique is specified and is false.");
reshardCmdTest.assertReshardCollOk(
    {reshardCollection: ns, key: {newKey: 1}, unique: false, numInitialChunks: 1}, 1);

jsTest.log(
    "Succeed if _presetReshardedChunks is provided and test commands are enabled (default).");
reshardCmdTest.assertReshardCollOkWithPreset({reshardCollection: ns, key: {newKey: 1}},
                                             presetReshardedChunks);

presetReshardedChunks = createChunks(shardNames, "newKey", 0, numInitialDocs).map(chunk => {
    chunk["recipientShardId"] = chunk["shard"];
    delete chunk["shard"];
    return chunk;
});

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
assert.commandWorked(db.adminCommand({addShardToZone: shardNames[0], zone: newZoneName}));
reshardCmdTest.assertReshardCollOk({
    reshardCollection: ns,
    key: {newKey: 1},
    unique: false,
    collation: {locale: 'simple'},
    numInitialChunks: 1,
    zones: [{zone: newZoneName, min: {newKey: 5}, max: {newKey: 10}}]
},
                                   3);

jsTest.log("Succeed if resulting chunks all end up in one shard.");
reshardCmdTest.assertReshardCollOk({
    reshardCollection: ns,
    key: {newKey: 1},
    unique: false,
    collation: {locale: 'simple'},
    numInitialChunks: 1,
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
assert.commandWorked(db.adminCommand({addShardToZone: shardNames[0], zone: existingZoneName}));
assert.commandWorked(db.adminCommand(
    {updateZoneKeyRange: ns, min: {oldKey: 0}, max: {oldKey: 5}, zone: existingZoneName}));
reshardCmdTest.assertReshardCollOk({
    reshardCollection: ns,
    key: {oldKey: 1, newKey: 1},
    unique: false,
    collation: {locale: 'simple'},
    numInitialChunks: 1,
    zones: [{zone: existingZoneName, min: {oldKey: 0}, max: {oldKey: 5}}]
},
                                   3);

jsTest.log("Succeed with hashed shard key that provides enough cardinality.");
assert.commandWorked(
    db.adminCommand({shardCollection: ns, key: {a: "hashed"}, numInitialChunks: 5}));
assert.commandWorked(db.getCollection(collName).insert(
    Array.from({length: 10000}, () => ({a: new ObjectId(), b: new ObjectId()}))));
assert.commandWorked(
    db.adminCommand({reshardCollection: ns, key: {b: "hashed"}, numInitialChunks: 5}));
db.getCollection(collName).drop();
