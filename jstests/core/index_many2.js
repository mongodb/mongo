// Cannot implicitly shard accessed collections because of extra shard key index in sharded
// collection.
// @tags: [assumes_no_implicit_index_creation]

const t = db.index_many2;
t.drop();

t.save({x: 1});

assert.eq(1, t.getIndexKeys().length, "Expected a single default index.");

function make(n) {
    var x = {};
    x["x" + n] = 1;
    return x;
}

jsTestLog("Creating 1000 indexes.");

// Try to create 1000 indexes. Only 63 will succeed because 64 is the maximum number of indexes
// allowed on a collection.
for (let i = 1; i < 1000; i++) {
    // Cannot assert success because only 63 additional indexes will succeed.
    t.createIndex(make(i));
}

const indexKeys = t.getIndexKeys();
const num = indexKeys.length;
assert.eq(64, num, "Expected 64 keys, found: " + num);

// Drop one of the indexes.
const indexToDrop = indexKeys.filter(key => key._id !== 1)[num - 2];

jsTestLog("Dropping index: '" + tojson(indexToDrop) + "'");

t.dropIndex(indexToDrop);
assert.eq(num - 1, t.getIndexKeys().length, "After dropping an index, there should be 63 left.");

// Create another index.
const indexToCreate = {
    z: 1
};

jsTestLog("Creating an index: '" + tojson(indexToCreate) + "'");

t.createIndex(indexToCreate);
assert.eq(num, t.getIndexKeys().length, "Expected 64 indexes.");

// Drop all the indexes except the _id index.
jsTestLog("Dropping all indexes with wildcard '*'");

t.dropIndexes("*");
assert.eq(1, t.getIndexKeys().length, "Expected only one index after dropping indexes via '*'");

jsTestLog("Test index_many2.js complete.");
