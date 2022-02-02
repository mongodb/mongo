/**
 * Tests that hex encoding of keyValue attributes of DuplicateKeyErrorInfo does not cause any issues
 * in mixed version configurations while upgrading from 5.0 to 6.0 and during a corresponding
 * downgrade.
 *
 * TODO SERVER-57052: This test is specific to the 5.0 <=> 6.0 upgrade/downgrade and should be
 * deleted after branching for 6.0.
 */
(function() {
"use strict";
load("jstests/multiVersion/libs/multi_cluster.js");  // For upgradeCluster.
const collName = jsTestName();
/**
 * Perform insert and upsert operations in order to trigger DuplicateKeyError and verify that the
 * hexadecimal encoding of 'keyValue' works correctly even if some nodes in the cluster are
 * version 5.0.
 */
function testDuplicateKeyError(db, isUpgraded) {
    const coll = db[collName];
    assert.commandWorked(coll.remove({}));

    // Inserting document into the collection in order to later force duplicate key error.
    assert.commandWorked(coll.insert({_id: "mongo/1"}));

    // Verify that insertion fails when the document with the same key already exists.
    const rawResponse = db.runCommand({
        insert: coll.getName(),
        documents: [{_id: "mongo/1"}],
    });
    assert.eq(rawResponse.writeErrors.length, 1, rawResponse);
    assert.eq(rawResponse.writeErrors[0].code, ErrorCodes.DuplicateKey);

    if (isUpgraded) {
        assert.eq(rawResponse.writeErrors[0].code, ErrorCodes.DuplicateKey, rawResponse);
        assert.eq(rawResponse.writeErrors[0].hexEncoded, [true], rawResponse);
        assert.eq(rawResponse.writeErrors[0].keyValue, {_id: "41454335450a8614010b"}, rawResponse);
        assert(rawResponse.writeErrors[0].hasOwnProperty("collation"), rawResponse);
    }

    // Verify that document update succeeds when the existing key is upserted.
    assert.commandWorked(coll.updateOne({_id: "mongo/1"}, {$set: {a: 1}}, {upsert: true}));
}

// Start a sharded cluster in which all processes are of the 5.0 binVersion.
const st = new ShardingTest({
    shards: 1,
    rs: {nodes: 2, binVersion: "last-lts"},
    other: {
        mongosOptions: {binVersion: "last-lts"},
        configOptions: {binVersion: "last-lts"},
    },
});
const mongos = st.s;

// Setup a collection with the collation. Custom collation key is added in order to recreate the
// desired test scenario, where the keyValue of DuplicateKeyValueError contains ICU collation keys
// which are invalid UTF-8.
let db = mongos.getDB("db");
assert.commandWorked(db.createCollection(collName, {collation: {locale: "en", strength: 2}}));

// Upgrade config servers to latest version.
st.upgradeCluster("latest", {
    upgradeShards: false,
    upgradeConfigs: true,
    upgradeMongos: false,
});
testDuplicateKeyError(db, false);

st.upgradeCluster("latest", {
    upgradeShards: true,
    upgradeConfigs: false,
    upgradeMongos: false,
});
testDuplicateKeyError(db, false);

// Upgrade the mongos.
st.upgradeCluster("latest", {
    upgradeShards: false,
    upgradeConfigs: false,
    upgradeMongos: true,
});
db = st.s.getDB("db");
testDuplicateKeyError(db, true);

// Downgrade the mongos.
st.upgradeCluster("last-lts", {
    upgradeShards: false,
    upgradeConfigs: false,
    upgradeMongos: true,
});
db = st.s.getDB("db");
testDuplicateKeyError(db, false);

st.upgradeCluster("last-lts", {
    upgradeShards: true,
    upgradeConfigs: true,
    upgradeMongos: false,
});
testDuplicateKeyError(db, false);
st.stop();
})();
