/**
 * Mongos has special targeting behavior for createIndex, dropIndex, and collMod:
 *
 * - If called on an unsharded collection, the request is routed only to the primary shard.
 * - If called on a sharded collection, the request is broadcast to shards with chunks.
 *
 * This test verifies this behavior.
 *
 * Shuts down shard0, which also shuts down the config server. Tests mongos targeting, which won't
 * be affected by a config shard.
 * @tags: [config_shard_incompatible]
 */

// This test shuts down a shard's node and because of this consistency checking
// cannot be performed on that node, which causes the consistency checker to fail.
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;
TestData.skipCheckingIndexesConsistentAcrossCluster = true;
TestData.skipCheckShardFilteringMetadata = true;

// Helper function that runs listIndexes against shards to check for the existence of an index.
function checkShardIndexes(indexKey, shardsWithIndex, shardsWithoutIndex) {
    function shardHasIndex(indexKey, shard) {
        const res = shard.getDB(dbName).runCommand({listIndexes: collName});
        if (res.code === ErrorCodes.NamespaceNotFound) {
            return [res, false];
        }
        assert.commandWorked(res);
        for (let index of res.cursor.firstBatch) {
            if (index.key.hasOwnProperty(indexKey)) {
                return [res, true];
            }
        }
        return [res, false];
    }

    for (let shard of shardsWithIndex) {
        let [listIndexesRes, foundIndex] = shardHasIndex(indexKey, shard);
        assert(foundIndex,
               "expected to see index with key " + indexKey + " in listIndexes response from " +
                   shard + ": " + tojson(listIndexesRes));
    }

    for (let shard of shardsWithoutIndex) {
        let [listIndexesRes, foundIndex] = shardHasIndex(indexKey, shard);
        assert(!foundIndex,
               "expected not to see index with key " + indexKey + " in listIndexes response from " +
                   shard + ": " + tojson(listIndexesRes));
    }
}

// Helper function that runs listCollections against shards to check for the existence of a
// collection option.
function checkShardCollOption(optionKey, optionValue, shardsWithOption, shardsWithoutOption) {
    function shardHasOption(optionKey, optionValue, shard) {
        const res = shard.getDB(dbName).runCommand({listCollections: 1, filter: {name: collName}});
        assert.commandWorked(res);
        if (res.cursor.firstBatch.length === 0) {
            return [res, false];
        }
        assert.eq(1, res.cursor.firstBatch.length);
        if (friendlyEqual(res.cursor.firstBatch[0].options[optionKey], optionValue)) {
            return [res, true];
        }
        return [res, false];
    }

    for (let shard of shardsWithOption) {
        let [listCollsRes, foundOption] = shardHasOption(optionKey, optionValue, shard);
        assert(foundOption,
               "expected to see option " + optionKey + " in listCollections response from " +
                   shard + ": " + tojson(listCollsRes));
    }

    for (let shard of shardsWithoutOption) {
        let [listOptionsRes, foundOption] = shardHasOption(optionKey, optionValue, shard);
        assert(!foundOption,
               "expected not to see option " + optionKey + " in listCollections response from " +
                   shard + ": " + tojson(listOptionsRes));
    }
}

const dbName = "test";
const collName = "foo";
const ns = dbName + "." + collName;

var st = new ShardingTest(
    {shards: {rs0: {nodes: 1}, rs1: {nodes: 1}, rs2: {nodes: 1}}, other: {config: 3}});

assert.commandWorked(st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.name}));

// When creating index or setting a collection option on an unsharded collection, only the
// primary shard is affected.

assert.commandWorked(st.s.getDB(dbName).getCollection(collName).createIndex({"idx1": 1}));
checkShardIndexes("idx1", [st.shard0], [st.shard1, st.shard2]);

const validationOption1 = {
    dummyField1: {$type: "string"}
};
assert.commandWorked(st.s.getDB(dbName).runCommand({
    collMod: collName,
    validator: validationOption1,
    validationLevel: "moderate",
    validationAction: "warn"
}));
checkShardCollOption("validator", validationOption1, [st.shard0], [st.shard1, st.shard2]);

// After sharding the collection but before any migrations, only the primary shard has the
// index and collection option.
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {x: 1}}));
checkShardIndexes("idx1", [st.shard0], [st.shard1, st.shard2]);
checkShardCollOption("validator", validationOption1, [st.shard0], [st.shard1, st.shard2]);

// After a migration, only shards that own data for the collection have the index and collection
// option.
assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {x: 0}, to: st.shard1.shardName}));
checkShardIndexes("idx1", [st.shard0, st.shard1], [st.shard2]);
checkShardCollOption("validator", validationOption1, [st.shard0, st.shard1], [st.shard2]);

// Starting in v4.4, createIndex, reIndex, dropIndex, collMod only target the shards that own
// chunks for the collection (as supposed to all shards in the previous versions). The commands
// will retry on shard version errors, and only report overall success. That is, IndexNotFound
// errors from shards are ignored, and not included in the 'raw' shard responses.

var res;

// createIndex
res = st.s.getDB(dbName).getCollection(collName).createIndex({"idx2": 1});
assert.commandWorked(res);
assert.eq(undefined, res.raw[st.shard0.host], tojson(res));
assert.eq(res.raw[st.shard1.host].ok, 1, tojson(res));
assert.eq(undefined, res.raw[st.shard2.host], tojson(res));
checkShardIndexes("idx2", [st.shard1], [st.shard2]);

// dropIndex
res = st.s.getDB(dbName).getCollection(collName).dropIndex("idx1_1");
assert.eq(undefined, res.raw[st.shard0.host], tojson(res));
assert.eq(res.raw[st.shard1.host].ok, 1, tojson(res));
assert.eq(undefined, res.raw[st.shard2.host], tojson(res));
assert.commandWorked(res);
checkShardIndexes("idx1", [], [st.shard1, st.shard2]);

// collMod targets all shards, regardless of whether they have chunks. The shards that have no
// chunks for the collection will not be included in the responses.
const validationOption2 = {
    dummyField2: {$type: "string"}
};
res = st.s.getDB(dbName).runCommand({
    collMod: collName,
    validator: validationOption2,
    validationLevel: "moderate",
    validationAction: "warn"
});
assert.commandWorked(res);
checkShardCollOption("validator", validationOption2, [st.shard0, st.shard1], [st.shard2]);

// Check that errors from shards are aggregated correctly.

// If no shard returns success, then errors that are usually ignored should be reported.
res = st.s.getDB(dbName).getCollection("unshardedColl").dropIndex("nonexistentIndex");
assert.eq(res.raw[st.shard0.host].ok, 0, tojson(res));
assert.eq(res.code, res.raw[st.shard0.host].code, tojson(res));
assert.eq(res.codeName, res.raw[st.shard0.host].codeName, tojson(res));
assert.eq(res.code, ErrorCodes.NamespaceNotFound, tojson(res));
assert.eq("NamespaceNotFound", res.codeName, tojson(res));
assert.neq(null, res.errmsg, tojson(res));

// If all shards report the same error, the overall command error should be set to that error.
res = st.s.getDB(dbName).getCollection(collName).createIndex({});
assert.eq(undefined, res.raw[st.shard0.host], tojson(res));
assert.eq(undefined, res.raw[st.shard2.host], tojson(res));
assert.eq(res.raw[st.shard1.host].ok, 0, tojson(res));
assert.eq(res.code, res.raw[st.shard1.host].code, tojson(res));
assert.eq(res.codeName, res.raw[st.shard1.host].codeName, tojson(res));
assert.eq(res.code, ErrorCodes.CannotCreateIndex, tojson(res));
assert.eq("CannotCreateIndex", res.codeName, tojson(res));
assert.neq(null, res.errmsg, tojson(res));

// If all the non-ignorable errors reported by shards are the same, the overall command error
// should be set to that error.
res = st.s.getDB(dbName).getCollection(collName).createIndex({z: 1}, {unique: true});
assert.eq(undefined, res.raw[st.shard0.host], tojson(res));
assert.eq(res.raw[st.shard1.host].ok, 0, tojson(res));
assert.eq(null, res.raw[st.shard2.host], tojson(res));
assert.eq(ErrorCodes.CannotCreateIndex, res.raw[st.shard1.host].code, tojson(res));
assert.eq("CannotCreateIndex", res.raw[st.shard1.host].codeName, tojson(res));
assert.eq(res.code, ErrorCodes.CannotCreateIndex, tojson(res));
assert.eq("CannotCreateIndex", res.codeName, tojson(res));
assert.neq(null, res.errmsg, tojson(res));

st.rs0.stopSet();

// If we receive a non-ignorable error, it should be reported as the command error.
res = st.s.getDB(dbName).getCollection("unshardedColl").createIndex({"validIdx": 1});
assert.eq(res.raw[st.shard0.host].ok, 0, tojson(res));
assert.eq(res.code, res.raw[st.shard0.host].code, tojson(res));
assert.eq(res.codeName, res.raw[st.shard0.host].codeName, tojson(res));
// We might see 'HostUnreachable' the first time if the mongos's ReplicaSetMonitor does not yet
// know that the shard is down.
assert(res.code === ErrorCodes.HostUnreachable ||
           res.code === ErrorCodes.FailedToSatisfyReadPreference,
       tojson(res));
assert(res.codeName === "HostUnreachable" || res.codeName === "FailedToSatisfyReadPreference",
       tojson(res));

// If some shard returns a non-ignorable error, it should be reported as the command error, even
// if other shards returned ignorable errors.
res = st.s.getDB(dbName).getCollection(collName).createIndex({"validIdx": 1});
assert.eq(undefined, res.raw[st.shard0.host], tojson(res));
assert.eq(res.ok, 1, tojson(res));
assert.eq(res.raw[st.shard1.host].ok, 1, tojson(res));  // gets created on shard that owns chunks
assert.eq(undefined, res.raw[st.shard2.host], tojson(res));  // shard does not own chunks

st.stop();
