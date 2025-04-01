/**
 * Test index specs in an old format are cleaned up on FCV transition to 9.0.
 */
import "jstests/multiVersion/libs/multi_rs.js";

import {rewriteCatalogTable} from "jstests/disk/libs/wt_file_helper.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const databaseName = jsTestName();
const collectionName = "coll";

// Rewrites the durable catalog for the given namespace, injecting the "ns" field to the
// "md.indexes.spec" object, to simulate 4.2 metadata.
function rewriteNodeCatalog(rst, node, namespace) {
    jsTestLog("Rewriting durable catalog in: " + node.dbpath);
    rst.stop(node, /*signal=*/ null, /*opts=*/ null, {forRestart: true, waitpid: true});

    function durableCatalogModFn(entry) {
        if (entry.ns == namespace) {
            entry.md.indexes.forEach((index) => {
                index.spec.ns = namespace;
            });
        }
    }
    rewriteCatalogTable(node, durableCatalogModFn);

    rst.restart(node);
}

function assertExpectedCollectionIndexSpecWithNamespaceField(node, expected) {
    let nodeColl = node.getDB(databaseName)[collectionName];

    let listCatalogRes = nodeColl.aggregate([{$listCatalog: {}}]).toArray();
    let filterRes = listCatalogRes.filter((entry) => entry.md.ns == nodeColl.getFullName());
    assert.eq(filterRes.length, 1);
    let collEntry = filterRes[0];

    function errMsg() {
        if (expected)
            return `Node: ${node.host}:${
                node.port}. Expected 'ns' field to be found in index spec in : ${
                tojson(collEntry)}`;
        else
            return `Node: ${node.host}:${
                node.port}. Expected 'ns' field to NOT be found in index spec in : ${
                tojson(collEntry)}`;
    }

    collEntry.md.indexes.forEach((index) => {
        assert.eq(index.spec.hasOwnProperty("ns"), expected, errMsg);
    });
}

function rewriteReplicaSetCatalog(rst, fullCollName) {
    rst.awaitReplication();

    // Rewrite durable catalog on all replicas.
    rst.nodes.forEach((node) => {
        if (node == rst.getPrimary()) {
            rst.stepUp(rst.getSecondary());
        }
        // This implicitly tests startup behavior with an old data format. The namespace field must
        // remain a valid index spec field until we are certain (post 9.0) that it is no longer
        // possible for it to be present.
        rewriteNodeCatalog(rst, node, fullCollName);
    });

    rst.awaitSecondaryNodes();
}

function testForReplicaSet() {
    const rst = new ReplSetTest({nodes: 2, nodeOptions: {binVersion: 'latest'}});
    rst.startSet();
    rst.initiate();

    let db = rst.getPrimary().getDB(databaseName);
    let coll = db[collectionName];

    // Ensure we are in last LTS FCV (8.0).
    assert.commandWorked(
        rst.getPrimary().adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}));

    assert.commandWorked(db.runCommand({
        createIndexes: coll.getName(),
        indexes: [{key: {a: 1}, name: 'a_1'}],
    }));

    rewriteReplicaSetCatalog(rst, coll.getFullName());

    // Check metadata is in 4.2 format.
    rst.nodes.forEach((node) => {
        assertExpectedCollectionIndexSpecWithNamespaceField(node, true);
    });

    assert.commandWorked(
        rst.getPrimary().adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));

    // Check metadata has been cleaned up.
    rst.nodes.forEach((node) => {
        assertExpectedCollectionIndexSpecWithNamespaceField(node, false);
    });

    rst.stopSet();
}

function testForShardedCluster() {
    const st = new ShardingTest({shards: 2, mongos: 1, config: 1, rs: {nodes: 2}});

    // Ensure we are in last LTS FCV (8.0).
    assert.commandWorked(
        st.s.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}));

    const fullCollName = databaseName + "." + collectionName;

    assert.commandWorked(
        st.s.adminCommand({enableSharding: databaseName, primaryShard: st.shard0.shardName}));
    assert.commandWorked(st.s.adminCommand({shardCollection: fullCollName, key: {x: 1}}));

    jsTestLog(st.s.getDB("config").collections.find().toArray());
    jsTestLog(st.s.getDB("config").chunks.find().toArray());

    assert.commandWorked(st.s.getDB(databaseName).runCommand({
        createIndexes: collectionName,
        indexes: [{key: {a: 1}, name: 'a_1'}],
    }));

    rewriteReplicaSetCatalog(st.rs0, fullCollName);

    st.rs0.nodes.forEach((node) => {
        assertExpectedCollectionIndexSpecWithNamespaceField(node, true);
    });

    // Verify shard1 has no collection.
    assert.eq(
        assert
            .commandWorked(st.rs1.getPrimary().getDB(databaseName).runCommand({listCollections: 1}))
            .cursor.firstBatch.length,
        0);

    assert.commandWorked(
        st.s.adminCommand({moveChunk: fullCollName, find: {x: 0}, to: st.shard1.shardName}));

    // Verify shard1 has cloned collection.
    assert.eq(
        assert
            .commandWorked(st.rs1.getPrimary().getDB(databaseName).runCommand({listCollections: 1}))
            .cursor.firstBatch.length,
        1);

    // Check old format metadata is not cloned. Even if setFCV happens concurrently with migrations,
    // the deprecated metadata cannot be created elsewhere by cloning. Thus the setFCV step is
    // guaranteed to completely remove the deprecated fields.
    st.rs1.nodes.forEach((node) => {
        assertExpectedCollectionIndexSpecWithNamespaceField(node, false);
    });

    // Double check old format metadata is still on shard0 (primary shard always owns the
    // collection).
    st.rs0.nodes.forEach((node) => {
        assertExpectedCollectionIndexSpecWithNamespaceField(node, true);
    });

    // Implicitly verify that metadata consistency checks do not fail due to old metadata being in
    // shard0 but not being in shard1.
    st.stop();
}

testForReplicaSet();
testForShardedCluster();
