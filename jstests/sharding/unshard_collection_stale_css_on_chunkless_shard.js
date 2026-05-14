/**
 * SERVER-123346: UnshardCollection leaves stale metadata in CSS on chunkless shards.
 *
 * Companion jstest for the TLA+ spec at:
 *   src/mongo/tla_plus/Sharding/UnshardChunklessCSS/
 *
 * Setup mirrors the spec's participant graph:
 *   - config server (global catalog)
 *   - donor / chunkless shard (had chunks pre-unshard, ends up with zero)
 *   - recipient shard (toShard: ends up owning the single post-unshard chunk)
 *
 * Repro:
 *   1. shardCollection(ns) across two shards so both carry CSS entries with the same UUID.
 *   2. unshardCollection(ns, toShard=shard1). Resharding under the hood mints a fresh UUID and
 *      installs it on the recipient + config server. The donor shard (now chunkless) is left out.
 *   3. Read each shard's CSS via getShardVersion(..., fullMetadata: true) and compare its UUID to
 *      the config server's `config.collections._id == ns` UUID.
 *
 * Expected behavior with the bug present (master today): the chunkless shard's CSS UUID does NOT
 * match the config server's, demonstrating the leftover state the ticket describes. The test
 * documents this as the bug rather than failing hard, so it is safe to land alongside the spec
 * before SPM-3961 lands. Once resharding becomes commit-shard-authoritative and refreshes the
 * chunkless shard's CSS, flip kAllowStaleCSS to false and the strict invariant takes over.
 *
 * @tags: [
 *  requires_fcv_80,
 *  featureFlagUnshardCollection,
 *  assumes_balancer_off,
 * ]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";

// Set to false once SPM-3961 (commit-shard-authoritative resharding) lands; the test will then
// fail-loud if the chunkless shard's CSS UUID still diverges from the config server's.
const kAllowStaleCSS = true;

const st = new ShardingTest({mongos: 1, shards: 2});

const dbName = jsTestName();
const collName = "foo";
const ns = dbName + "." + collName;

const mongos = st.s0;
const shard0Name = st.shard0.shardName;
const shard1Name = st.shard1.shardName;

assert.commandWorked(mongos.adminCommand({enableSharding: dbName, primaryShard: shard0Name}));

const coll = mongos.getDB(dbName)[collName];
assert.commandWorked(coll.createIndex({oldKey: 1}));
assert.commandWorked(mongos.adminCommand({shardCollection: ns, key: {oldKey: 1}}));

// Split + move so both shards own chunks (both will hold CSS for ns).
assert.commandWorked(mongos.adminCommand({split: ns, middle: {oldKey: 0}}));
assert.commandWorked(mongos.adminCommand({moveChunk: ns, find: {oldKey: -1}, to: shard0Name}));
assert.commandWorked(mongos.adminCommand({moveChunk: ns, find: {oldKey: 10}, to: shard1Name}));

// Force both shards to refresh so their CSS is populated with the pre-unshard UUID.
function flushAndGetShardCSS(shardConn, namespace) {
    assert.commandWorked(shardConn.adminCommand({_flushRoutingTableCacheUpdates: namespace}));
    const res = assert.commandWorked(
        shardConn.adminCommand({getShardVersion: namespace, fullMetadata: true}));
    return res;
}

flushAndGetShardCSS(st.shard0, ns);
flushAndGetShardCSS(st.shard1, ns);

const preUnshardConfig = mongos.getDB("config").collections.findOne({_id: ns});
assert.neq(null, preUnshardConfig, "config.collections must have an entry for " + ns);
const preUnshardUUID = preUnshardConfig.uuid;
jsTest.log("Pre-unshard UUID at config server: " + tojson(preUnshardUUID));

// Run unshardCollection targeting shard1. shard0 becomes the chunkless shard.
assert.commandWorked(mongos.adminCommand({unshardCollection: ns, toShard: shard1Name}));

const postUnshardConfig = mongos.getDB("config").collections.findOne({_id: ns});
assert.neq(null, postUnshardConfig);
assert.eq(true, postUnshardConfig.unsplittable, "collection should be unsplittable post-unshard");
const postUnshardUUID = postUnshardConfig.uuid;
jsTest.log("Post-unshard UUID at config server: " + tojson(postUnshardUUID));

// Resharding mints a fresh UUID, so config-server UUID must differ from the pre-unshard one.
assert.neq(tojson(preUnshardUUID),
           tojson(postUnshardUUID),
           "unshardCollection should have minted a fresh UUID at the config server");

// Read CSS on both shards. Refresh first so anything the shard would normally pull is in cache.
const shard0CSS = flushAndGetShardCSS(st.shard0, ns);
const shard1CSS = flushAndGetShardCSS(st.shard1, ns);

jsTest.log("shard0 (chunkless) CSS: " + tojson(shard0CSS));
jsTest.log("shard1 (recipient)  CSS: " + tojson(shard1CSS));

// Helper: pull the collection UUID out of the fullMetadata response, tolerant to the shape used
// by both routing-info-aware and pre-authoritative shards.
function extractCssUUID(svRes) {
    if (svRes.metadata && svRes.metadata.uuid !== undefined) {
        return svRes.metadata.uuid;
    }
    if (svRes.metadata && svRes.metadata.collVersion && svRes.metadata.collVersion.uuid !== undefined) {
        return svRes.metadata.collVersion.uuid;
    }
    return undefined;
}

const shard0CssUUID = extractCssUUID(shard0CSS);
const shard1CssUUID = extractCssUUID(shard1CSS);

// Recipient (shard1) must agree with the config server. That part of the commit path is correct
// today and must keep working post-fix.
if (shard1CssUUID !== undefined) {
    assert.eq(tojson(postUnshardUUID),
              tojson(shard1CssUUID),
              "recipient shard's CSS UUID must match the post-unshard config server UUID");
}

// Chunkless shard (shard0). This is the bug site.
if (shard0CssUUID === undefined) {
    // Acceptable: CSS got cleared entirely. This is one of the two states the spec's
    // CSSUUIDMatchesConfigServerOrIsCleared invariant allows.
    jsTest.log("Chunkless shard has no CSS metadata for " + ns + " (acceptable state)");
} else if (tojson(shard0CssUUID) === tojson(postUnshardUUID)) {
    jsTest.log("Chunkless shard's CSS UUID already matches config server (SPM-3961 may have landed)");
} else {
    const msg = "SERVER-123346 reproduced: chunkless shard " + shard0Name +
                " has stale CSS UUID " + tojson(shard0CssUUID) +
                " for " + ns + "; config server has " + tojson(postUnshardUUID);
    if (kAllowStaleCSS) {
        jsTest.log(msg);
        jsTest.log("Documented bug; not failing the test. Flip kAllowStaleCSS=false after SPM-3961.");
    } else {
        assert(false, msg);
    }
}

st.stop();
