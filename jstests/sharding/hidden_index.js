/**
 * Test to validate that a shard key index cannot be hidden if it cannot be dropped.
 *  @tags: [
 *   requires_fcv_60,
 * ]
 */

(function() {
'use strict';
load("jstests/libs/index_catalog_helpers.js");  // For IndexCatalogHelpers.findByName.

// Test to validate the correct behaviour of hiding an index in a sharded cluster with a shard key.
function validateHiddenIndexBehaviour() {
    let index_type = 1;
    let index_name = "a_" + index_type;
    assert.commandWorked(coll.createIndex({"a": index_type}));

    let idxSpec = IndexCatalogHelpers.findByName(coll.getIndexes(), index_name);
    assert.eq(idxSpec.hidden, undefined);

    assert.commandWorked(coll.hideIndex(index_name));
    idxSpec = IndexCatalogHelpers.findByName(coll.getIndexes(), index_name);
    assert(idxSpec.hidden);

    assert.commandWorked(coll.unhideIndex(index_name));
    idxSpec = IndexCatalogHelpers.findByName(coll.getIndexes(), index_name);
    assert.eq(idxSpec.hidden, undefined);

    assert.commandWorked(coll.dropIndex(index_name));
    assert.commandWorked(coll.createIndex({"a": index_type}, {hidden: true}));

    idxSpec = IndexCatalogHelpers.findByName(coll.getIndexes(), index_name);
    assert(idxSpec.hidden);
    assert.commandWorked(coll.dropIndexes());
}

// Check that command will fail when we try to hide the only shard key index of the collection
function validateOneShardKeyHiddenIndexBehaviour() {
    assert.commandFailedWithCode(coll.hideIndex({skey: 1}), ErrorCodes.InvalidOptions);
    assert.commandFailedWithCode(coll.hideIndex("skey_1"), ErrorCodes.InvalidOptions);
    assert.commandFailedWithCode(
        testDb.runCommand({"collMod": coll.getName(), "index": {"name": "skey_1", "hidden": true}}),
        ErrorCodes.InvalidOptions);
}

// Check that command will fail when we try to hide or drop an index that is the last shard key
// index of the collection
function validateDifferentHiddenIndexesBehaviour() {
    // Create index on skey
    assert.commandWorked(coll.createIndex({skey: 1, anotherkey: 1}));

    // Check that is possible to hide a shard key index using its key pattern
    assert.commandWorked(coll.hideIndex({skey: 1}));
    assert.commandWorked(coll.unhideIndex({skey: 1}));

    // Check that is possible to hide a shard key index using its name
    assert.commandWorked(testDb.runCommand(
        {"collMod": coll.getName(), "index": {"name": "skey_1", "hidden": true}}));

    assert.commandFailedWithCode(
        testDb.runCommand(
            {"collMod": coll.getName(), "index": {"name": "skey_1_anotherkey_1", "hidden": true}}),
        ErrorCodes.InvalidOptions);

    assert.commandFailed(coll.dropIndex("skey_1_anotherkey_1"));
}

// Configure initial sharded cluster
const st = new ShardingTest({shards: 2});
const mongos = st.s;
const testDb = mongos.getDB("test");
const coll = testDb.getCollection("foo");

// Enable sharding at collection 'foo' and create a new shard key
assert.commandWorked(st.s.adminCommand({enableSharding: testDb.getName()}));

// Crate a new shard key
assert.commandWorked(
    st.s.adminCommand({shardcollection: testDb.getName() + '.' + coll.getName(), key: {skey: 1}}));

validateHiddenIndexBehaviour();
validateOneShardKeyHiddenIndexBehaviour();
validateDifferentHiddenIndexesBehaviour();

st.stop();
})();
