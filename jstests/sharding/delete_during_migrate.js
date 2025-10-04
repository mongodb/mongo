/**
 * Test migrating a big chunk while deletions are happening within that chunk. Test is slightly
 * non-deterministic, since removes could happen before migrate starts. Protect against that by
 * making chunk very large.
 *
 * This test is labeled resource intensive because its total io_write is 88MB compared to a median
 * of 5MB across all sharding tests in wiredTiger.
 * @tags: [resource_intensive]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";
import {isSlowBuild} from "jstests/sharding/libs/sharding_util.js";

let st = new ShardingTest({
    shards: 2,
    mongos: 1,
    rs: {nodes: 2, setParameter: {defaultConfigCommandTimeoutMS: 5 * 60 * 1000}},
});

let dbname = "test";
let coll = "foo";
let ns = dbname + "." + coll;

assert.commandWorked(st.s0.adminCommand({enablesharding: dbname, primaryShard: st.shard1.shardName}));

let t = st.s0.getDB(dbname).getCollection(coll);

let bulk = t.initializeUnorderedBulkOp();
const numDocs = isSlowBuild(st.s0) ? 150000 : 200000;
jsTest.log("Testing with " + numDocs + " documents");
for (let i = 0; i < numDocs; i++) {
    bulk.insert({a: i});
}
assert.commandWorked(bulk.execute());

// enable sharding of the collection. Only 1 chunk.
t.createIndex({a: 1});

assert.commandWorked(st.s0.adminCommand({shardcollection: ns, key: {a: 1}}));

// start a parallel shell that deletes things
let join = startParallelShell("db." + coll + ".remove({});", st.s0.port);

// migrate while deletions are happening
const res = st.s0.adminCommand({moveChunk: ns, find: {a: 1}, to: st.getOther(st.getPrimaryShard(dbname)).name});
if (res.code == ErrorCodes.CommandFailed && res.errmsg.includes("timed out waiting for the catch up completion")) {
    jsTest.log(
        "Ignoring the critical section timeout error since this test deletes " +
            numDocs +
            " documents in the chunk being migrated " +
            tojson(res),
    );
} else {
    assert.commandWorked(res);
}

join();

st.stop();
