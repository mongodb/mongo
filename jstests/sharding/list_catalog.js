/**
 * Tests that $listCatalog only returns entries from chunk-owning shards.
 *
 * TODO (SERVER-64980): Extend test for collectionless $listCatalog once it only returns entries
 * from chunk-owning shards.
 *
 * @tags: [
 *   requires_fcv_60,
 * ]
 */
(function() {
'use strict';

const st = new ShardingTest({shards: 2});

const db = st.s.getDB(jsTestName());
assert.commandWorked(st.s.adminCommand({enableSharding: db.getName()}));
st.ensurePrimaryShard(db.getName(), st.shard0.shardName);

const coll = db.coll;
assert.commandWorked(st.s.adminCommand({shardCollection: coll.getFullName(), key: {_id: 1}}));

// Split at {_id: 1}, moving {_id: 0} to shard0 and {_id: 1} to shard1.
assert.commandWorked(st.splitAt(coll.getFullName(), {_id: 1}));
assert.commandWorked(
    st.s.adminCommand({moveChunk: coll.getFullName(), find: {_id: 0}, to: st.shard0.shardName}));
assert.commandWorked(
    st.s.adminCommand({moveChunk: coll.getFullName(), find: {_id: 1}, to: st.shard1.shardName}));

const checkResult = function(res) {
    for (const entry of res) {
        assert.eq(entry.db, db.getName());
        assert.eq(entry.name, coll.getName());
        assert.eq(entry.ns, coll.getFullName());
        assert.eq(entry.type, "collection");
        assert.eq(entry.md.indexes.length, entry.md.options.clusteredIndex ? 0 : 1);
    }
};

let res = coll.aggregate([{$listCatalog: {}}]).toArray();
jsTestLog('$listCatalog with multiple chunk-owning shards: ' + tojson(res));
assert.eq(res.length, 2);
assert(res.some((entry) => entry.shard === st.shard0.shardName));
assert(res.some((entry) => entry.shard === st.shard1.shardName));
checkResult(res);

// Move {_id: 0} to shard1 so that shard0 does not own any chunks for the collection.
assert.commandWorked(
    st.s.adminCommand({moveChunk: coll.getFullName(), find: {_id: 0}, to: st.shard1.shardName}));

res = coll.aggregate([{$listCatalog: {}}]).toArray();
jsTestLog('$listCatalog with one chunk-owning shard: ' + tojson(res));
assert.eq(res.length, 1);
assert.eq(res[0].shard, st.shard1.shardName);
checkResult(res);

st.stop();
})();
