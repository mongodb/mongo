/**
 * Regression test: a bulkWrite on a normal collection "db.X" must not be silently redirected to its
 * "db.system.buckets.X" counterpart just because the client also lists
 * the buckets namespace in nsInfo. Previously the sharded router's unified write executor translated
 * the op namespace to the buckets namespace based purely on the buckets namespace being present in
 * the routing context, which a client fully controls via nsInfo -- letting a write authorized
 * against db.X land on the system.buckets.X namespace it was never authorized for.
 *
 * @tags: [
 *   # The router-side guard asserted here does not exist in older binaries, so this test cannot
 *   # pass in multiversion suites that run a mixed-version cluster.
 *   requires_fcv_90,
 * ]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({shards: 2});

const kDbName = "test";
const kColl = "coll";
const kBucket = "system.buckets.coll";

assert.commandWorked(
    st.s0.adminCommand({enableSharding: kDbName, primaryShard: st.shard0.shardName}),
);

const db = st.getDB(kDbName);

// "coll" is an ordinary (non-timeseries) collection. Shard it so it is tracked in the global
// catalog -- this is the collection the user is authorized to write.
assert.commandWorked(db.createCollection(kColl));
assert.commandWorked(st.s0.adminCommand({shardCollection: `${kDbName}.${kColl}`, key: {_id: 1}}));

// Craft a bulkWrite that lists both "coll" and "system.buckets.coll" in nsInfo, but only references
// "coll" (nsInfo index 0) in its ops. Before the fix, the router redirected this insert to
// system.buckets.coll.
const doc = {_id: 1, x: 42};
assert.commandWorked(
    db.adminCommand({
        bulkWrite: 1,
        ops: [{insert: 0, document: doc}],
        nsInfo: [{ns: `${kDbName}.${kColl}`}, {ns: `${kDbName}.${kBucket}`}],
    }),
);

// The document must land in the intended collection...
assert.eq(
    1,
    db.getCollection(kColl).find({_id: 1}).itcount(),
    "insert did not land in the user collection",
);
// ...and NOT in the system.buckets namespace. Direct find() on a system.buckets namespace is
// rejected by the server, so check via listCollections that the namespace was never created.
assert.eq(
    0,
    db.getCollectionInfos({name: kBucket}).length,
    "insert was misrouted to the system.buckets namespace",
);

st.stop();
