/**
 * Verifies that drop operations clear the in-memory collection filtering metadata
 * (CollectionShardingState) on the config server, so that a subsequent transition from a dedicated
 * config server to an embedded config shard does not surface stale shard versions.
 *
 * Regression test for SERVER-91944: DDL coordinators previously broadcast drop to every shard but
 * skipped the config server, leaving its CSS populated with the pre-drop placement version. After
 * `transitionFromDedicatedConfigServer`, that stale CSS could be served to routers targeting the
 * now-embedded data shard.
 *
 * @tags: [
 *   requires_fcv_83,
 *   requires_persistence,
 *   featureFlagTransitionToCatalogShard,
 * ]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({mongos: 1, shards: 2, config: 1, rs: {nodes: 1}});

const dbName = "dropClearsCssDB";
const collName = "coll";
const ns = dbName + "." + collName;

// Primary shard is a real data shard (NOT the config server) so the drop path is the production
// one: DDL coordinator fans out to participant shards but historically not to the config server.
assert.commandWorked(st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {x: 1}}));
assert.commandWorked(st.s.adminCommand({split: ns, middle: {x: 0}}));
assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {x: 0}, to: st.shard1.shardName}));

assert.commandWorked(st.s.getDB(dbName).getCollection(collName).insert([{x: -1}, {x: 1}]));

// Capture the config-server primary BEFORE the drop. We will probe its CSS directly via a
// direct connection; the config server is not yet a data shard so it should never have been told
// about this namespace -- but with the SERVER-91944 bug it would have observed routing traffic on
// internal paths and cached a non-UNKNOWN shard version.
const configPrimary = st.configRS.getPrimary();

function getCssShardVersion(conn, namespace) {
    const res = conn.adminCommand({getShardVersion: namespace, fullMetadata: true});
    assert.commandWorked(res);
    return res;
}

// Drop the collection through the canonical DDL path.
assert.commandWorked(st.s.getDB(dbName).runCommand({drop: collName}));

// Promote the config server to also serve as a data shard. With the bug, any stale CSS entry
// for `ns` survives this transition; with the fix, the config server either never cached metadata
// for `ns` or refreshed/cleared its CSS as part of the transition.
assert.commandWorked(st.s.adminCommand({transitionFromDedicatedConfigServer: 1}));

// Re-shard a fresh collection on the same namespace with a different key to make any stale CSS
// observable: a stale shard version on the config-now-data shard would conflict with the new
// placement and produce StaleConfig on direct reads.
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {y: 1}}));
assert.commandWorked(st.s.getDB(dbName).getCollection(collName).insert({y: 42}));

// The config-shard's CSS for `ns` must NOT report a pre-drop placement. Acceptable post-fix
// states: UNKNOWN (never tracked), or a version consistent with the new collection's epoch.
const postTransitionCss = getCssShardVersion(configPrimary, ns);
const newCollUuid = st.s.getDB("config").collections.findOne({_id: ns}).uuid;

if (postTransitionCss.global && postTransitionCss.global !== "UNKNOWN") {
    // If CSS is populated it must reflect the post-drop collection, not the dropped one.
    assert(
        postTransitionCss.metadata && postTransitionCss.metadata.uuid !== undefined,
        "Expected fullMetadata on populated CSS: " + tojson(postTransitionCss),
    );
    assert.eq(
        postTransitionCss.metadata.uuid,
        newCollUuid,
        "Config server retained stale CSS from dropped collection: " + tojson(postTransitionCss),
    );
}

// End-to-end witness: a router-driven read after the transition must succeed without resurfacing
// stale-config errors that escape retries (the user-visible symptom of the bug).
assert.eq(1, st.s.getDB(dbName).getCollection(collName).find({}).itcount());

st.stop();
