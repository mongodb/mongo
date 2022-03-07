// Cannot implicitly shard accessed collections because of extra shard key index in sharded
// collection.
// @tags: [assumes_no_implicit_index_creation]

(function() {
'use strict';

load("jstests/libs/clustered_collections/clustered_collection_util.js");

const collectionIsClustered = ClusteredCollectionUtil.areAllCollectionsClustered(db.getMongo());

const t = db.index_many2;
t.drop();

assert.commandWorked(t.insert({_id: 1, x: 1}));

assert.eq(1, t.getIndexKeys().length, "Expected a single default index.");

function make(n) {
    let x = {};
    x["x" + n] = 1;
    return x;
}

// This should match the constant in IndexCatalogImpl::kMaxNumIndexesAllowed.
// A clustered collection doesn't actually have an index on _id, although the index is reported
// for backward compatibility. As a consequence on a clustered collection it's possible to create
// kMaxNumIndexesAllowed indexes in addition to the index on _id.
const maxNumIndexesAllowed = collectionIsClustered ? 65 : 64;

jsTestLog("Creating " + (maxNumIndexesAllowed - 1) + " indexes.");

// Only 63 will succeed because 64 is the maximum number of indexes allowed on a collection.
assert.soon(() => {
    // Index creation May fail due to stepdowns and shutdowns. Keep trying until we reach the
    // server limit for indexes in a collection.
    try {
        const numCurrentIndexes = t.getIndexKeys().length;
        for (let i = numCurrentIndexes + 1; i <= maxNumIndexesAllowed; i++) {
            const key = make(i);
            const res = assert.commandWorked(t.createIndex(key));
            jsTestLog('createIndex: ' + tojson(key) + ': ' + tojson(res));
        }
        assert.eq(t.getIndexKeys().length, maxNumIndexesAllowed);
        return true;
    } catch (e) {
        jsTest.log("Failed to create indexes: " + e);
        return false;
    }
});

const indexKeys = t.getIndexKeys();
const num = indexKeys.length;
assert.eq(maxNumIndexesAllowed, num, "Expected 64 keys, found: " + num);

assert.commandFailedWithCode(t.createIndex({y: 1}), ErrorCodes.CannotCreateIndex);

// Drop one of the indexes.
const indexToDrop = indexKeys.filter(key => key._id !== 1)[num - 2];

jsTestLog("Dropping index: '" + tojson(indexToDrop) + "'");

t.dropIndex(indexToDrop);
assert.eq(num - 1,
          t.getIndexKeys().length,
          "After dropping an index, there should be " + (maxNumIndexesAllowed - 1) + " left.");

// Create another index.
const indexToCreate = {
    z: 1
};

jsTestLog("Creating an index: '" + tojson(indexToCreate) + "'");

t.createIndex(indexToCreate);
assert.eq(num, t.getIndexKeys().length, "Expected " + maxNumIndexesAllowed + " indexes.");

// Drop all the indexes except the _id index.
jsTestLog("Dropping all indexes with wildcard '*'");

t.dropIndexes("*");
assert.eq(1, t.getIndexKeys().length, "Expected only one index after dropping indexes via '*'");

jsTestLog("Test index_many2.js complete.");
})();
