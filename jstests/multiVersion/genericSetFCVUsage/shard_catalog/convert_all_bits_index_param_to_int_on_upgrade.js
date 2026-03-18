/**
 * Tests that a 2d index spec with a non-integer 'bits' value written in lastLTS FCV is converted to
 * an integer 'bits' value on FCV upgrade.
 *
 * TODO SERVER-120350: Remove this test once v9.0 becomes last LTS
 */

import "jstests/multiVersion/libs/multi_cluster.js";

import {ShardingTest} from "jstests/libs/shardingtest.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

// Skip test if lastLTSFCV is not 8.0. In that case, the bits parameter would already be stored as
// an integer, so there is no upgrade path to test.
if (lastLTSFCV !== "8.0") {
    jsTest.log.info(
        "Skipping test: lastLTSFCV is not 8.0 anymore. This test can be removed once v9.0 becomes last LTS.",
    );
    quit();
}

const dbName = jsTestName();
const collName = "collTest";

// Retrieves the 'bits' value for the 2d index with the given index name.
function getIndexBits(node, indexName = "loc_2d") {
    const indexes = node
        .getDB(dbName)
        [collName].aggregate([{$indexStats: {}}])
        .toArray();
    const indexEntry = indexes.find((index) => index.name === indexName);
    assert(indexEntry, "Expected index called " + indexName + " to be found");
    return indexEntry.spec.bits;
}

function testForReplicaSet() {
    const rst = new ReplSetTest({nodes: 2, nodeOptions: {binVersion: "last-lts"}});
    rst.startSet();
    rst.initiate();

    let db = rst.getPrimary().getDB(dbName);

    // Ensure we are in last LTS FCV (8.0).
    assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}));

    assert.commandWorked(
        db.runCommand({
            createIndexes: collName,
            indexes: [{key: {loc: "2d"}, name: "loc_2d", bits: 11.6}],
        }),
    );

    // Ensure changes are replicated to all nodes before asserting.
    rst.awaitReplication();

    // Check bits is stored as a non-integer in lastLTS FCV.
    rst.nodes.forEach((node) => {
        const bitsValue = getIndexBits(node);
        assert.eq(11.6, bitsValue, "Expected bits=11.6 (non-integer) to be stored as-is in lastLTS FCV");
        assert(!Number.isInteger(bitsValue));
    });

    // Upgrade all nodes to latest binary version.
    rst.upgradeSet({binVersion: "latest"});
    db = rst.getPrimary().getDB(dbName);

    // The upgrade process has not been called yet, so the bits parameter should still be stored as a non-integer.
    rst.nodes.forEach((node) => {
        const bitsValue = getIndexBits(node);
        assert.eq(11.6, bitsValue, "Expected bits=11.6 (non-integer) to be stored as-is in lastLTS FCV");
        assert(!Number.isInteger(bitsValue));
    });

    // Upgrade to latest FCV.
    assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));

    // Ensure changes are replicated to all nodes before asserting.
    rst.awaitReplication();

    // Check bits have been converted to an integer.
    rst.nodes.forEach((node) => {
        const bitsValue = getIndexBits(node, "loc_2d");
        assert.eq(11, bitsValue, "Expected bits=11 (integer) to be converted to an integer");
        assert(Number.isInteger(bitsValue));
    });

    // From now on, any new 2d index will have an integer 'bits' value.
    assert.commandWorked(
        db.runCommand({
            createIndexes: collName,
            indexes: [{key: {loc2: "2d"}, name: "loc2_2d", bits: 9.2}],
        }),
    );
    rst.awaitReplication();
    rst.nodes.forEach((node) => {
        const bitsValue = getIndexBits(node, "loc2_2d");
        assert.eq(9, bitsValue, "Expected bits=9 (integer) to be stored as an integer for index loc2_2d");
        assert(Number.isInteger(bitsValue));
    });

    rst.stopSet();
}

function testForShardedCluster() {
    const st = new ShardingTest({
        shards: 2,
        mongos: 1,
        config: 1,
        rs: {nodes: 2},
        mongosOptions: {binVersion: "last-lts"},
        rsOptions: {binVersion: "last-lts"},
    });

    // Ensure we are in last LTS FCV (8.0).
    assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}));

    let db = st.s.getDB(dbName);
    const fullCollName = dbName + "." + collName;

    assert.commandWorked(st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));
    assert.commandWorked(st.s.adminCommand({shardCollection: fullCollName, key: {x: 1}}));

    assert.commandWorked(
        db.runCommand({
            createIndexes: collName,
            indexes: [{key: {loc: "2d"}, name: "loc_2d", bits: 11.6}],
        }),
    );

    // Ensure changes are replicated to all nodes of shard0 before asserting.
    st.rs0.awaitReplication();

    // Check bits is stored as a non-integer in lastLTS FCV.
    st.rs0.nodes.forEach((node) => {
        assert.eq(11.6, getIndexBits(node), "Expected bits=11.6 (non-integer) to be stored as-is in lastLTS FCV");
    });

    // Upgrade all nodes to latest binary version.
    st.upgradeCluster("latest", {waitUntilStable: true});
    db = st.s.getDB(dbName);

    // The upgrade process has not been called yet, so the bits parameter should still be stored as a non-integer.
    st.rs0.nodes.forEach((node) => {
        const bitsValue = getIndexBits(node);
        assert.eq(11.6, bitsValue, "Expected bits=11.6 (non-integer) to be stored as-is in lastLTS FCV");
        assert(!Number.isInteger(bitsValue));
    });

    // Upgrade to latest FCV.
    assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));

    // Ensure changes are replicated to all nodes before asserting.
    st.rs0.awaitReplication();

    // Ensure bits is stored as an integer.
    st.rs0.nodes.forEach((node) => {
        const bitsValue = getIndexBits(node, "loc_2d");
        assert.eq(11, bitsValue, "Expected bits=11 (integer) to be stored as an integer");
        assert(Number.isInteger(bitsValue));
    });

    // From now on, any new 2d index will have an integer 'bits' value.
    assert.commandWorked(
        db.runCommand({
            createIndexes: collName,
            indexes: [{key: {loc2: "2d"}, name: "loc2_2d", bits: 9.2}],
        }),
    );
    st.rs0.awaitReplication();
    st.rs0.nodes.forEach((node) => {
        const bitsValue = getIndexBits(node, "loc2_2d");
        assert.eq(9, bitsValue, "Expected bits=9 (integer) to be stored as an integer for index loc2_2d");
        assert(Number.isInteger(bitsValue));
    });

    st.stop();
}

testForReplicaSet();
testForShardedCluster();
