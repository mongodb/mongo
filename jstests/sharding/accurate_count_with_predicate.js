/**
 * Tests that Collection.count(), when run with a predicate (not a "fast count"), filters out
 * orphan documents. This is intended to test the fix for SERVER-3645.
 *
 * The test works by sharding a collection, and then inserting orphan documents directly into one
 * of the shards. It then runs a count() and ensures that the orphan documents are not counted
 * twice.
 */
(function() {
"use strict";

const st = new ShardingTest({shards: 2});
const shard0Coll = st.shard0.getCollection("test.slowcount");
const num = 10;
const middle = num / 2;

function getNthDocument(n) {
    return {_id: n, one: 1, x: n};
}

// Shard the collection. Shard 0 will get keys from [0, middle) and shard 1 will get everything
// from [middle, num).
assert.commandWorked(st.s.getDB("admin").runCommand({enableSharding: "test"}));
st.ensurePrimaryShard("test", st.shard0.name);
st.shardColl(shard0Coll.getName(), {x: 1}, {x: middle}, {x: middle + 1}, "test", true);

// Insert some docs.
for (let i = 0; i < num; i++) {
    assert.writeOK(st.getDB("test").slowcount.insert(getNthDocument(i)));
}

// Insert some orphan documents to shard 0. These are just documents outside the range
// which shard 0 owns.
for (let i = middle + 1; i < middle + 3; i++) {
    assert.writeOK(shard0Coll.insert(getNthDocument(i)));
}

// Run a count on the whole collection. The orphaned documents on shard 0 shouldn't be double
// counted.
assert.eq(st.getDB("test").slowcount.count({one: 1}), num);

st.stop();
})();
