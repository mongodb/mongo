/**
 * Tests that checkMetadataConsistency tolerates 'expireAfterSeconds' numeric type mismatches
 * (e.g. double 3600.5 vs int 3600) when FCV < 9.0, and surfaces them as genuine inconsistencies
 * once FCV reaches 9.0.
 *
 * Pre-7.3 nodes persisted 'expireAfterSeconds' using the BSON type of the user-supplied value
 * (e.g. 3600 -> int, 3600.5 -> double). Starting with 7.3 the value is always truncated to
 * integer, so a mixed-version cluster could end up with the same index stored as double 3600.5
 * on one shard and integer 3600 on another. SERVER-120253 normalises all on-disk values during
 * the FCV upgrade to 9.0. This test uses rewriteCatalogTable to inject a legacy double value on
 * one shard, simulating data left over from a pre-7.3 node.
 *
 * @tags: [
 *   requires_fcv_90,
 *   requires_persistence,
 *   requires_sharding,
 *   requires_wiredtiger,
 * ]
 */

import {rewriteCatalogTable} from "jstests/disk/libs/wt_file_helper.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const dbName = jsTestName();
const collName = "coll";
const fullNs = dbName + "." + collName;
const indexName = "ttl_index";

const st = new ShardingTest({shards: 2, mongos: 1, config: 1, rs: {nodes: 1}});
const mongos = st.s;
const adminDB = mongos.getDB("admin");

assert.commandWorked(
    mongos.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}),
);
assert.commandWorked(mongos.adminCommand({shardCollection: fullNs, key: {x: 1}}));

const coll = mongos.getDB(dbName)[collName];
for (let i = -5; i < 5; i++) {
    assert.commandWorked(coll.insert({x: i}));
}
assert.commandWorked(mongos.adminCommand({split: fullNs, middle: {x: 0}}));
assert.commandWorked(
    mongos.adminCommand({moveChunk: fullNs, find: {x: 1}, to: st.shard1.shardName}),
);

// Integer value stored by a 7.3+ node.
const kExpireAfterSeconds = 3600;

assert.commandWorked(
    coll.createIndex({a: 1}, {name: indexName, expireAfterSeconds: kExpireAfterSeconds}),
);

// Helper function to rewrite shard0's durable catalog to store 'expireAfterSeconds' as double,
// simulating data written by a pre-7.3 node.
function injectDoubleOnShard0() {
    const node = st.rs0.getPrimary();
    st.rs0.stop(node, null, null, {forRestart: true, waitpid: true});
    rewriteCatalogTable(node, (entry) => {
        if (entry.ns === fullNs) {
            entry.md.indexes.forEach((index) => {
                if (index.spec.name === indexName) {
                    // Adding a fractional part is required because the JS shell coerces integer-valued numbers to int32
                    // during BSON serialisation.
                    index.spec.expireAfterSeconds = kExpireAfterSeconds + 0.1;
                }
            });
        }
    });
    st.rs0.restart(node);
    st.rs0.awaitSecondaryNodes();
    st.rs0.waitForPrimary();
}

// FCV < 9.0: type mismatch should be tolerated.
jsTest.log.info("Setting FCV to lastLTSFCV", {lastLTSFCV});
assert.commandWorked(
    adminDB.runCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}),
);

// shard0 carries legacy double (kExpireAfterSeconds + 0.1) written by a pre-7.3 node;
// shard1 has integer kExpireAfterSeconds. CMC must not report a false positive.
injectDoubleOnShard0();

let inconsistencies = adminDB
    .checkMetadataConsistency({checkIndexes: 1})
    .toArray()
    .filter((i) => i.type === "InconsistentIndex");
assert.eq(
    0,
    inconsistencies.length,
    "Expected no false positive for numeric type mismatch at FCV < 9.0",
    {
        inconsistencies,
    },
);

// FCV >= 9.0: type mismatch is a genuine inconsistency that CMC should surface.
// On backport branches this section should be removed: the C++ code there always sets
// tolerateExpireAfterSecondsTypeMismatch = true so there is no strict mode.
jsTest.log.info("Upgrading FCV to latestFCV", {latestFCV});
assert.commandWorked(
    adminDB.runCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}),
);

// SERVER-120253 normalizes on-disk values on upgrade; re-inject the double to verify
// CMC is strict at FCV 9.0.
injectDoubleOnShard0();

inconsistencies = adminDB
    .checkMetadataConsistency({checkIndexes: 1})
    .toArray()
    .filter((i) => i.type === "InconsistentIndex");
assert.gt(
    inconsistencies.length,
    0,
    "Expected type mismatch to be flagged as InconsistentIndex at FCV 9.0",
);

// Clean up so the post-test CMC hook finds a consistent cluster.
mongos.getDB(dbName).dropDatabase();

st.stop();
