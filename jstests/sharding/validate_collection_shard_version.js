/*
 * Verify that the validate command respects the Shard Versioning Protocol
 *
 *  @tags: [assumes_balancer_off]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

var st = new ShardingTest({mongos: 2, shards: 2, rs: {nodes: 1}});

var s0 = st.s0;
var s1 = st.s1;

const kDbName = "test";
const kCollName = "foo";
const kNsColl = kDbName + "." + kCollName;

const s0Db = s0.getDB(kDbName);
const s1Db = s1.getDB(kDbName);
const s0Coll = s0Db.getCollection(kCollName);
const s1Coll = s1Db.getCollection(kCollName);

assert.commandWorked(s0.adminCommand({enableSharding: kDbName, primaryShard: st.shard0.shardName}));
assert.commandWorked(s0.adminCommand({shardCollection: kNsColl, key: {_id: 1}}));

assert.commandWorked(s0Coll.insert({_id: 2}));

// Ensure that mongos0 becomes stale.
assert.commandWorked(s1.adminCommand({moveChunk: kNsColl, find: {_id: 2}, to: st.shard1.shardName}));

// Insert corrupted document
configureFailPoint(st.shard1, "corruptDocumentOnInsert", {}, "alwaysOn");
assert.commandWorked(s1Coll.insert({_id: 1}));
configureFailPoint(st.shard1, "corruptDocumentOnInsert", {}, "off");

// Up-to-date mongos detects the corrupted document.
var res1 = s1Coll.validate();
assert.eq(res1.valid, false, "Validate returned valid true when expected false: " + tojson(res1));

// The stale mongos performs a refresh and detects the corrupted document.
var res0 = s0Coll.validate();
assert.eq(res0.valid, false, "Validate returned valid true when expected false: " + tojson(res0));

// Drop the database so that we don't fail the automatic validation which runs when the test exits.
st.s.getDB(kDbName).getCollection(kCollName).drop();

st.stop();
