/**
 * Tests that index projections are persisted in the originally submitted form, as opposed to
 * normalized form, in the catalog. Exercises the fix for SERVER-67446.
 * @tags: [
 *   # Uses index building in background
 *   requires_background_index,
 *   requires_fcv_50,
 *   requires_sharding,
 * ]

 */
(function() {
"use strict";

const st = new ShardingTest({shards: 3, rs: {nodes: 1}});
const dbName = jsTestName();
const collName = "test";
const ns = dbName + "." + collName;

assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
st.ensurePrimaryShard(dbName, st.shard0.shardName);
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: "hashed"}}));

const testDB = st.s.getDB("test");

const kKeyPattern = {
    "$**": 1
};

const kProjectionDoc = {
    "name": 0,
    "type": 0,
    "a.b": 0,
    "_id": 1
};

// Returns the index description object from getIndexes() for a given index by name, or an empty
// object if not found.
function getIndex(indexName) {
    let indexes = testDB[collName].getIndexes();
    for (var index of indexes) {
        if (index.name == indexName) {
            return index;
        }
    }
    return {};
}

// Creates a wildcard index with a simple unnormalized wildcardProjection.
assert.commandWorked(testDB[collName].createIndex(
    kKeyPattern, {name: "wc_a_b", wildcardProjection: kProjectionDoc}));

// Verifies the wildcardProjection is stored unnormalized in the catalog.
const catalog = getIndex("wc_a_b");
assert.eq(catalog.wildcardProjection,
          kProjectionDoc,
          "Expected unnormalized wildcardProjection in catalog but got something else.");

st.stop();
})();
