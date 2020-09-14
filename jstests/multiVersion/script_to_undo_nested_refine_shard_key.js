//
// Verifies the script to fix a collection's metadata after a refineCollectionShardKey stores
// boundaries in an invalid format.
//

(function() {
"use strict";

function findNewlyAddedFields(prevShardKey, currentShardKey) {
    const prevFields = Object.keys(prevShardKey);
    const currentFields = Object.keys(currentShardKey);
    assert.lt(
        prevFields.length,
        currentFields.length,
        "Given prevShardKey: " + tojson(prevShardKey) +
            " should have fewer fields than the current shard key: " + tojson(currentShardKey));

    let newFields = [];
    for (let i = 0; i < currentFields.length; i++) {
        if (i >= prevFields.length) {
            newFields.push(currentFields[i]);
        } else {
            assert.eq(prevFields[i],
                      currentFields[i],
                      "Given prevShardKey: " + tojson(prevShardKey) +
                          " is not a prefix of the current shard key: " + tojson(currentShardKey) +
                          ". Fields at index: " + i + " are not the same.");
        }
    }
    return newFields;
}

// Undoes the effect of a shard key refine affected by the issue tracked in
// https://jira.mongodb.org/browse/SERVER-50750. Reverts the collection's shard key to the
// pre-refine key, reverts all chunks and tags to have their pre-refine bounds, and changes the
// collection's epoch to force automatic refreshes.
//
// Parameters:
// 'configConn' - A mongo connection to the config server replica set (direct to the primary or a
// replica set url).
// 'affectedNs' - A string representing the namespace of the sharded collection.
// 'prevShardKey' - An object representing the shard key before the shard key refine.
//
// Example:
// To undo a refine from shard key {a: 1, "b.c": 1} to {a: 1, "b.c": 1, "d.e.f": 1, g: 1} on
// collection `shardedCollection.foo`, in a shell connected directly to the config server primary,
// invoke this function as follows:
// ```
//  undoShardKeyRefine(db.getMongo(), "shardedCollection.foo", {a: 1, "b.c": 1});
// ```
function undoShardKeyRefine(configConn, affectedNs, prevShardKey) {
    assert(typeof affectedNs === "string");
    assert(typeof prevShardKey === "object");

    // Get the current shard key from config.collections and figure out which fields were newly
    // added and need to be removed.
    const currentShardKey = configConn.getDB("config").collections.findOne({_id: affectedNs}).key;
    const newlyAddedFields = findNewlyAddedFields(prevShardKey, currentShardKey);

    // In a transaction (to prevent corrupting metadata in the event of a crash), remove each newly
    // added shard key field from every chunk and tag, revert to the previous shard key in the
    // collection entry, and change the collection's epoch to force refreshes.

    const session = configConn.startSession();
    const sessionDB = session.getDatabase("config");
    session.startTransaction();

    let fieldsToUnset = [];
    for (let newFieldName of newlyAddedFields) {
        fieldsToUnset.push("min." + newFieldName.split('.')[0]);
        fieldsToUnset.push("max." + newFieldName.split('.')[0]);
    }
    const newEpoch = ObjectId();
    const chunksUpdatePipeline = [{$set: {lastmodEpoch: newEpoch}}, {$unset: fieldsToUnset}];
    const tagsUpdatePipeline = [{$unset: fieldsToUnset}];

    print("Updating collection entry to use epoch: " + tojson(newEpoch));
    assert.commandWorked(sessionDB.collections.update(
        {_id: affectedNs}, {$set: {lastmodEpoch: newEpoch, key: prevShardKey}}));

    print("Removing newly added shard key fields from chunk boundaries, new fields: " +
          tojson(newlyAddedFields));
    assert.commandWorked(sessionDB.chunks.update(
        {ns: affectedNs}, chunksUpdatePipeline, false /* upsert */, true /* multi */));

    print("Removing newly added shard key fields from tag boundaries, new fields: " +
          tojson(newlyAddedFields));
    assert.commandWorked(sessionDB.tags.update(
        {ns: affectedNs}, tagsUpdatePipeline, false /* upsert */, true /* multi */));

    session.commitTransaction();
}

function verifyMetadata(conn, ns, shardKey, prevEpoch) {
    const collection = conn.getDB("config").collections.findOne({_id: ns});
    assert.eq(collection.key, shardKey, tojson(collection));
    assert(prevEpoch, tojson(prevEpoch));
    assert.neq(collection.lastmodEpoch, prevEpoch, tojson(collection));

    const chunks = conn.getDB("config").chunks.find({ns: ns}).toArray();
    assert(chunks.length > 0, tojson(chunks));
    chunks.forEach(chunk => {
        assert(chunk.min && chunk.max, tojson(chunk));
        assert.sameMembers(Object.keys(chunk.min), Object.keys(shardKey));
        assert.sameMembers(Object.keys(chunk.max), Object.keys(shardKey));
    });

    const tags = conn.getDB("config").tags.find({ns: ns}).toArray();
    assert(tags.length > 0, tojson(tags));
    tags.forEach(tag => {
        assert(tag.min && tag.max, tojson(tag));
        assert.sameMembers(Object.keys(tag.min), Object.keys(shardKey));
        assert.sameMembers(Object.keys(tag.max), Object.keys(shardKey));
    });
}

function verifyBasicCrud(conn, ns) {
    const coll = conn.getCollection(ns);

    assert.commandWorked(coll.insert({a: 1, b: 2}));
    assert.eq(coll.find({a: 1, b: 2}).itcount(), 1);
    assert.eq(coll.find().itcount(), 1);

    assert.commandWorked(coll.update({a: 1}, {$set: {x: 1}}));
    assert.eq(coll.find({a: 1, b: 2, x: 1}).itcount(), 1);
    assert.eq(coll.find().itcount(), 1);

    assert.commandWorked(coll.remove({x: 1}));
    assert.eq(coll.find().itcount(), 0);
}

// Test against the version of the server that includes the refine shard key issue described in
// SERVER-50750.
const versionWithSERVER50750Issue = "4.4.1";
const st = new ShardingTest({
    mongos: 1,
    other: {
        mongosOptions: {binVersion: versionWithSERVER50750Issue},
        configOptions: {binVersion: versionWithSERVER50750Issue},
        rsOptions: {binVersion: versionWithSERVER50750Issue},
    }
});

//
// Verify the procedure to undo a broken shard key refine works in the following cases:
//

// For a shard key without nested fields refined to include more non nested fields.
(() => {
    const dbName = "dbNormalToNormal";
    const refinedNs = dbName + ".refinedColl";

    assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
    st.ensurePrimaryShard(dbName, st.shard0.shardName);
    assert.commandWorked(st.s.adminCommand({addShardToZone: st.shard0.shardName, zone: 'zone_1'}));

    // Sharded by {a: 1}
    assert.commandWorked(st.s.adminCommand({shardCollection: refinedNs, key: {a: 1}}));
    assert.commandWorked(st.s.adminCommand({split: refinedNs, middle: {a: 0}}));
    assert.commandWorked(st.s.adminCommand(
        {updateZoneKeyRange: refinedNs, min: {a: MinKey}, max: {a: 0}, zone: 'zone_1'}));
    assert.commandWorked(st.s.adminCommand(
        {updateZoneKeyRange: refinedNs, min: {a: 10}, max: {a: MaxKey}, zone: 'zone_1'}));

    // Refined to {a: 1, b: 1, c: 1}
    assert.commandWorked(st.s.getCollection(refinedNs).createIndex({a: 1, b: 1, c: 1}));
    assert.commandWorked(
        st.s.adminCommand({refineCollectionShardKey: refinedNs, key: {a: 1, b: 1, c: 1}}));

    const prevEpoch = st.s.getDB("config").collections.findOne({_id: refinedNs}).lastmodEpoch;

    undoShardKeyRefine(st.configRS.getPrimary(), refinedNs, {a: 1});

    verifyMetadata(st.s, refinedNs, {a: 1}, prevEpoch);
    verifyBasicCrud(st.s, refinedNs);
})();

// For a shard key without nested fields refined to include nested fields.
(() => {
    const dbName = "dbNormalToNested";
    const refinedNs = dbName + ".refinedColl";

    assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
    st.ensurePrimaryShard(dbName, st.shard0.shardName);
    assert.commandWorked(st.s.adminCommand({addShardToZone: st.shard0.shardName, zone: 'zone_1'}));

    // Sharded by {a: 1, b: 1}
    assert.commandWorked(st.s.adminCommand({shardCollection: refinedNs, key: {a: 1, b: 1}}));
    assert.commandWorked(st.s.adminCommand({split: refinedNs, middle: {a: 0, b: 0}}));
    assert.commandWorked(st.s.adminCommand(
        {updateZoneKeyRange: refinedNs, min: {a: MinKey}, max: {a: 0, b: 0}, zone: 'zone_1'}));
    assert.commandWorked(st.s.adminCommand(
        {updateZoneKeyRange: refinedNs, min: {a: 10, b: 0}, max: {a: MaxKey}, zone: 'zone_1'}));

    // Refined to {a: 1, b: 1, "c.d.e": 1, f: 1}
    assert.commandWorked(st.s.getCollection(refinedNs).createIndex({a: 1, b: 1, "c.d.e": 1, f: 1}));
    assert.commandWorked(st.s.adminCommand(
        {refineCollectionShardKey: refinedNs, key: {a: 1, b: 1, "c.d.e": 1, f: 1}}));

    const prevEpoch = st.s.getDB("config").collections.findOne({_id: refinedNs}).lastmodEpoch;

    undoShardKeyRefine(st.configRS.getPrimary(), refinedNs, {a: 1, b: 1});

    verifyMetadata(st.s, refinedNs, {a: 1, b: 1}, prevEpoch);
    verifyBasicCrud(st.s, refinedNs);
})();

// For a shard key with nested fields refined to include non nested fields.
(() => {
    const dbName = "compareDBNestedToNormal";
    const refinedNs = dbName + ".refinedColl";

    assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
    st.ensurePrimaryShard(dbName, st.shard0.shardName);
    assert.commandWorked(st.s.adminCommand({addShardToZone: st.shard0.shardName, zone: 'zone_1'}));

    // Sharded by {"a.b": 1}
    assert.commandWorked(st.s.adminCommand({shardCollection: refinedNs, key: {"a.b": 1}}));
    assert.commandWorked(st.s.adminCommand({split: refinedNs, middle: {"a.b": 0}}));
    assert.commandWorked(st.s.adminCommand(
        {updateZoneKeyRange: refinedNs, min: {"a.b": MinKey}, max: {"a.b": 0}, zone: 'zone_1'}));
    assert.commandWorked(st.s.adminCommand(
        {updateZoneKeyRange: refinedNs, min: {"a.b": 10}, max: {"a.b": MaxKey}, zone: 'zone_1'}));

    // Refined to {"a.b": 1, c: 1, d: 1}
    assert.commandWorked(st.s.getCollection(refinedNs).createIndex({"a.b": 1, c: 1, d: 1}));
    assert.commandWorked(
        st.s.adminCommand({refineCollectionShardKey: refinedNs, key: {"a.b": 1, c: 1, d: 1}}));

    const prevEpoch = st.s.getDB("config").collections.findOne({_id: refinedNs}).lastmodEpoch;

    undoShardKeyRefine(st.configRS.getPrimary(), refinedNs, {"a.b": 1});

    verifyMetadata(st.s, refinedNs, {"a.b": 1}, prevEpoch);
    verifyBasicCrud(st.s, refinedNs);
})();

// For a shard key with nested fields refined to include more nested fields.
(() => {
    const dbName = "dbNestedToNested";
    const refinedNs = dbName + ".refinedColl";

    assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
    st.ensurePrimaryShard(dbName, st.shard0.shardName);
    assert.commandWorked(st.s.adminCommand({addShardToZone: st.shard0.shardName, zone: 'zone_1'}));

    // Sharded by {"a.b": 1}
    assert.commandWorked(st.s.adminCommand({shardCollection: refinedNs, key: {"a.b": 1}}));
    assert.commandWorked(st.s.adminCommand({split: refinedNs, middle: {"a.b": 0}}));
    assert.commandWorked(st.s.adminCommand(
        {updateZoneKeyRange: refinedNs, min: {"a.b": MinKey}, max: {"a.b": 0}, zone: 'zone_1'}));
    assert.commandWorked(st.s.adminCommand(
        {updateZoneKeyRange: refinedNs, min: {"a.b": 10}, max: {"a.b": MaxKey}, zone: 'zone_1'}));

    // Refined to {"a.b": 1, "c.d.e": 1, f: 1}
    assert.commandWorked(st.s.getCollection(refinedNs).createIndex({"a.b": 1, "c.d.e": 1, f: 1}));
    assert.commandWorked(st.s.adminCommand(
        {refineCollectionShardKey: refinedNs, key: {"a.b": 1, "c.d.e": 1, f: 1}}));

    const prevEpoch = st.s.getDB("config").collections.findOne({_id: refinedNs}).lastmodEpoch;

    undoShardKeyRefine(st.configRS.getPrimary(), refinedNs, {"a.b": 1});

    verifyMetadata(st.s, refinedNs, {"a.b": 1}, prevEpoch);
    verifyBasicCrud(st.s, refinedNs);
})();

st.stop();
})();
