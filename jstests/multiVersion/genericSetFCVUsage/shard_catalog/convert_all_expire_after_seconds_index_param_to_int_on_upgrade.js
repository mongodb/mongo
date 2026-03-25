/**
 * Tests that a TTL index spec with a non-integer 'expireAfterSeconds' value is converted to an
 * integer 'expireAfterSeconds' value on FCV upgrade.
 *
 * The float-to-integer truncation for 'expireAfterSeconds' has been enforced since v7.3. We
 * therefore use rewriteCatalogTable to directly inject a non-integer value into the durable
 * catalog, simulating data left over from a pre-v7.3 MongoDB version.
 *
 * TODO SERVER-120350: Remove this test once v9.0 becomes last LTS
 */

import {rewriteCatalogTable} from "jstests/disk/libs/wt_file_helper.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const dbName = jsTestName();
const collName = "collTest";
const fullNs = dbName + "." + collName;

// Retrieves the 'expireAfterSeconds' value for the TTL index with the given index name.
function getIndexExpireAfterSeconds(node, indexName = "ttl_index") {
    const indexes = node
        .getDB(dbName)
        [collName].aggregate([{$indexStats: {}}])
        .toArray();
    const indexEntry = indexes.find((index) => index.name === indexName);
    assert(indexEntry, "Expected index called " + indexName + " to be found");
    return indexEntry.spec.expireAfterSeconds;
}

// Rewrites the durable catalog on a single node, changing 'expireAfterSeconds' for the given
// index from an integer to a non-integer value, simulating data written by a pre-v7.3 MongoDB version.
function rewriteNodeCatalog(rst, node, ns, indexName, nonIntValue) {
    jsTest.log.info("Rewriting durable catalog", {node: node.dbpath, ns, indexName, nonIntValue});
    rst.stop(node, /*signal=*/ null, /*opts=*/ null, {forRestart: true, waitpid: true});

    function durableCatalogModFn(entry) {
        if (entry.ns == ns) {
            entry.md.indexes.forEach((index) => {
                if (index.spec.name === indexName) {
                    index.spec.expireAfterSeconds = nonIntValue;
                }
            });
        }
    }
    rewriteCatalogTable(node, durableCatalogModFn);

    rst.restart(node);
}

// Rewrites the durable catalog on all nodes in a replica set, injecting a non-integer
// 'expireAfterSeconds' value for the given TTL index.
function rewriteReplicaSetCatalog(rst, ns, indexName, nonIntValue) {
    rst.awaitReplication();

    rst.nodes.forEach((node) => {
        if (node == rst.getPrimary()) {
            rst.stepUp(rst.getSecondary());
        }
        rewriteNodeCatalog(rst, node, ns, indexName, nonIntValue);
    });

    rst.awaitSecondaryNodes();
}

function testForReplicaSet() {
    const rst = new ReplSetTest({nodes: 2});
    rst.startSet();
    rst.initiate();

    let db = rst.getPrimary().getDB(dbName);

    // Set lastLTS FCV (8.0) to simulate the environment where non-integer expireAfterSeconds
    // values could reside on disk.
    assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}));

    // Create a TTL index with an integer expireAfterSeconds (the current behavior).
    assert.commandWorked(
        db.runCommand({
            createIndexes: collName,
            indexes: [{key: {a: 1}, name: "ttl_index"}],
            // indexes: [{key: {a: 1}, name: "ttl_index", expireAfterSeconds: 123}],
        }),
    );

    // Rewrite the catalog on all nodes to inject a non-integer expireAfterSeconds, simulating
    // data written by a pre-v7.3 MongoDB version.
    rewriteReplicaSetCatalog(rst, fullNs, "ttl_index", 123.456);

    // Check expireAfterSeconds is stored as a non-integer value after the catalog rewrite.
    rst.nodes.forEach((node) => {
        const expireAfterSeconds = getIndexExpireAfterSeconds(node);
        assert.eq(
            123.456,
            expireAfterSeconds,
            "Expected expireAfterSeconds to be stored as a non-integer after catalog rewrite",
        );
        assert(!Number.isInteger(expireAfterSeconds));
    });

    // Upgrade to latest FCV. This triggers the repair path that converts any non-integer
    // 'expireAfterSeconds' values to their integer equivalent.
    db = rst.getPrimary().getDB(dbName);
    assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));

    // Ensure changes are replicated to all nodes before asserting.
    rst.awaitReplication();

    // Check expireAfterSeconds has been converted to an integer.
    rst.nodes.forEach((node) => {
        const expireAfterSeconds = getIndexExpireAfterSeconds(node, "ttl_index");
        assert.eq(123, expireAfterSeconds, "Expected expireAfterSeconds to be converted to an integer");
        assert(Number.isInteger(expireAfterSeconds));
    });

    // Verify that any new TTL index will have its 'expireAfterSeconds' value truncated to an
    // integer.
    assert.commandWorked(
        db.runCommand({
            createIndexes: collName,
            indexes: [{key: {b: 1}, name: "ttl_index2", expireAfterSeconds: 456.789}],
        }),
    );
    rst.awaitReplication();
    rst.nodes.forEach((node) => {
        const expireAfterSeconds = getIndexExpireAfterSeconds(node, "ttl_index2");
        assert.eq(
            456,
            expireAfterSeconds,
            "Expected expireAfterSeconds to be stored as an integer for index ttl_index2",
        );
        assert(Number.isInteger(expireAfterSeconds));
    });

    rst.stopSet();
}

function testForShardedCluster() {
    const st = new ShardingTest({
        shards: 2,
        mongos: 1,
        config: 1,
        rs: {nodes: 2},
    });

    // Set lastLTS FCV (8.0) to simulate the environment where non-integer expireAfterSeconds
    // values could reside on disk.
    assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}));

    let db = st.s.getDB(dbName);

    assert.commandWorked(st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));
    assert.commandWorked(st.s.adminCommand({shardCollection: fullNs, key: {x: 1}}));

    // Create a TTL index with an integer expireAfterSeconds (the current behavior).
    assert.commandWorked(
        db.runCommand({
            createIndexes: collName,
            indexes: [{key: {a: 1}, name: "ttl_index", expireAfterSeconds: 123}],
        }),
    );

    // Rewrite the catalog on all nodes of shard0 to inject a non-integer expireAfterSeconds,
    // simulating data written by a pre-v7.3 MongoDB version.
    rewriteReplicaSetCatalog(st.shard0.rs, fullNs, "ttl_index", 123.456);

    // Check expireAfterSeconds is stored as a non-integer value after the catalog rewrite.
    st.shard0.rs.nodes.forEach((node) => {
        const expireAfterSeconds = getIndexExpireAfterSeconds(node);
        assert.eq(
            123.456,
            expireAfterSeconds,
            "Expected expireAfterSeconds to be stored as a non-integer after catalog rewrite",
        );
        assert(!Number.isInteger(expireAfterSeconds));
    });

    // Upgrade to latest FCV. This triggers the repair path that converts any non-integer
    // 'expireAfterSeconds' values to their integer equivalent.
    assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));

    // Ensure changes are replicated to all nodes before asserting.
    st.shard0.rs.awaitReplication();

    // Ensure expireAfterSeconds is stored as an integer.
    st.shard0.rs.nodes.forEach((node) => {
        const expireAfterSeconds = getIndexExpireAfterSeconds(node, "ttl_index");
        assert.eq(123, expireAfterSeconds, "Expected expireAfterSeconds to be converted to an integer");
        assert(Number.isInteger(expireAfterSeconds));
    });

    // Verify that any new TTL index will have its 'expireAfterSeconds' value truncated to an
    // integer.
    assert.commandWorked(
        st.s.getDB(dbName).runCommand({
            createIndexes: collName,
            indexes: [{key: {b: 1}, name: "ttl_index2", expireAfterSeconds: 456.789}],
        }),
    );
    st.shard0.rs.awaitReplication();
    st.shard0.rs.nodes.forEach((node) => {
        const expireAfterSeconds = getIndexExpireAfterSeconds(node, "ttl_index2");
        assert.eq(
            456,
            expireAfterSeconds,
            "Expected expireAfterSeconds to be stored as an integer for index ttl_index2",
        );
        assert(Number.isInteger(expireAfterSeconds));
    });

    st.stop();
}

testForReplicaSet();
testForShardedCluster();
