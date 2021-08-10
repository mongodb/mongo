/**
 * Tests long collection name support in sharded collections.
 *
 * @tags: [
 *   requires_fcv_51,
 * ]
 */

(function() {
"use strict";

load("jstests/libs/uuid_util.js");

const st = new ShardingTest({mongos: 1, config: 1, shards: {rs0: {nodes: 2}}});
const mongos = st.s;
const primaryNode = st.rs0.getPrimary();
const secondaryNode = st.rs0.getSecondary();
secondaryNode.setSecondaryOk();

var collIndex = 1;
function createShardedCollection() {
    const collNs = testDb + ".sharded_coll" + collIndex++;
    assert.commandWorked(mongos.adminCommand({shardCollection: collNs, key: {x: 1}}));
    return collNs;
}

function testDisabledLongNameSupport(collNs) {
    jsTestLog("Verify that the feature is disabled on collection '" + collNs + "'");

    // Verify that long name support is properly disabled
    const collConfigDoc = mongos.getDB("config").collections.findOne({_id: collNs});
    assert.eq(collConfigDoc.supportingLongName, undefined);

    for (const configDb of [primaryNode.getDB('config'), secondaryNode.getDB('config')]) {
        // Verify that the collection exists in the cached metadata
        const cachedCollDoc = configDb['cache.collections'].findOne({_id: collNs});
        assert.neq(cachedCollDoc, null);

        // Verify that the chunks collection with the proper namespace (i.e., based on the
        // collection namespace) exists in the cached metadata
        assert(configDb['cache.chunks.' + collNs].exists());
        assert(!configDb['cache.chunks.' + extractUUIDFromObject(cachedCollDoc.uuid)].exists());
    }
}

function testImplicitlyEnabledLongNameSupport(collNs) {
    jsTestLog("Verify that the feature is implicitly enabled on collection '" + collNs + "'");

    // Verify that long name support is properly disabled
    const collConfigDoc = mongos.getDB("config").collections.findOne({_id: collNs});
    assert.eq(collConfigDoc.supportingLongName, "implicitly_enabled");

    for (const configDb of [primaryNode.getDB('config'), secondaryNode.getDB('config')]) {
        // Verify that the collection exists in the cached metadata
        const cachedCollDoc = configDb['cache.collections'].findOne({_id: collNs});
        assert.neq(cachedCollDoc, null);

        // Verify that the chunks collection with the proper namespace (i.e., based on the
        // collection namespace) exists in the cached metadata
        assert(!configDb['cache.chunks.' + collNs].exists());
        assert(configDb['cache.chunks.' + extractUUIDFromObject(cachedCollDoc.uuid)].exists());
    }
}

const testDb = "test_db";
assert.commandWorked(mongos.adminCommand({enableSharding: testDb}));

//////////////////////////////
// Working with FCV 5.0 -- Long name support must be disabled on all collections.

assert.commandWorked(mongos.getDB("admin").runCommand({setFeatureCompatibilityVersion: "5.0"}));

const collCreatedWithFCV50 = createShardedCollection();
testDisabledLongNameSupport(collCreatedWithFCV50);

//////////////////////////////
// Working with FCV 5.1 -- Long name support must be explicitly enabled on new collections.

assert.commandWorked(mongos.getDB("admin").runCommand({setFeatureCompatibilityVersion: "5.1"}));

testDisabledLongNameSupport(collCreatedWithFCV50);

const collCreatedWithFCV51 = createShardedCollection();
testImplicitlyEnabledLongNameSupport(collCreatedWithFCV51);

st.stop();
})();
