/**
 * Tests long collection name support in sharded collections.
 *
 * @tags: [
 *   requires_fcv_51,
 * ]
 */
(function() {
"use strict";

const st = new ShardingTest({mongos: 1, config: 1, shards: 2});
const mongos = st.s;
const adminDB = mongos.getDB("admin");
const configDB = mongos.getDB("config");

const testDB = "test_db";
assert.commandWorked(mongos.adminCommand({enableSharding: testDB}));

function testDisabledLongNameSupport() {
    jsTestLog("Verify that the feature is disabled with FCV 5.0");

    assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: "5.0"}));

    // Create an entry into the config.collections collection
    const collNs = testDB + ".sharded_coll1";
    assert.commandWorked(mongos.adminCommand({shardCollection: collNs, key: {_id: 1}}));

    // Verify that support is disabled with FCV 5.0
    const collConfigDoc = configDB.collections.findOne({_id: collNs});
    assert.eq(collConfigDoc.supportingLongName, undefined);
}

function testImplicitlyEnabledLongNameSupport() {
    jsTestLog("Verify that the feature is implicitly enabled with FCV 5.1");

    assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: "5.1"}));

    // Create an entry into the config.collections collection
    const collNs = testDB + ".sharded_coll2";
    assert.commandWorked(mongos.adminCommand({shardCollection: collNs, key: {_id: 1}}));

    // Verify that long name support is implicitly enabled with FCV 5.1
    const collConfigDoc = configDB.collections.findOne({_id: collNs});
    assert.eq(collConfigDoc.supportingLongName, "implicitly_enabled");
}

testDisabledLongNameSupport();
testImplicitlyEnabledLongNameSupport();

st.stop();
})();
