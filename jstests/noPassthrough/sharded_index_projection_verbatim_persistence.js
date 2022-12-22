/**
 * Tests that index projections are persisted in the originally submitted form, as opposed to
 * normalized form, in the catalog consistently across all shards. Exercises the fix for
 * SERVER-67446.
 * @tags: [
 *   # Uses index building in background
 *   requires_background_index,
 *   requires_fcv_63,
 * ]

 */
(function() {
"use strict";

const st = new ShardingTest({shards: 3, rs: {nodes: 1}});
const dbName = "test";
const collName = "user";
const ns = dbName + "." + collName;

assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
st.ensurePrimaryShard(dbName, st.shard0.shardName);

assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: "hashed"}}));

const kProjectionDoc = {
    "name": 0,
    "type": 0,
    "a.b": 0,
    "_id": 1
};

// Creates a wildcard index with a wildcardProjection that normalization would change and verifies
// the persisted projection doc on each shard matches the original, unnormalized version.
const kWildcardIndexName = "wc_index";
st.s.getCollection(ns).createIndex({"$**": 1},
                                   {name: kWildcardIndexName, wildcardProjection: kProjectionDoc});
let shardCatalogs =
    st.s.getCollection(ns)
        .aggregate([
            {$listCatalog: {}},
            {$unwind: "$md.indexes"},
            {$match: {"md.indexes.spec.name": kWildcardIndexName}},
            {$project: {shard: 1, wildcardProjection: "$md.indexes.spec.wildcardProjection"}}
        ])
        .toArray();
assert.eq(shardCatalogs.length, 3, shardCatalogs);
for (const catEntry of shardCatalogs) {
    assert.eq(catEntry.wildcardProjection, kProjectionDoc, shardCatalogs);
}

// Creates a columnstore index with a columnstoreProjection that normalization would change and
// verifies the persisted projection doc on each shard matches the original, unnormalized version.
const kColumnstoreIndexName = "cs_index";
st.s.getCollection(ns).createIndex(
    {"$**": "columnstore"}, {name: kColumnstoreIndexName, columnstoreProjection: kProjectionDoc});
shardCatalogs =
    st.s.getCollection(ns)
        .aggregate([
            {$listCatalog: {}},
            {$unwind: "$md.indexes"},
            {$match: {"md.indexes.spec.name": kColumnstoreIndexName}},
            {$project: {shard: 1, columnstoreProjection: "$md.indexes.spec.columnstoreProjection"}}
        ])
        .toArray();
assert.eq(shardCatalogs.length, 3, shardCatalogs);
for (const catEntry of shardCatalogs) {
    assert.eq(catEntry.columnstoreProjection, kProjectionDoc, shardCatalogs);
}

st.stop();
})();
