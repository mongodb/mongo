// stats will return ok:1 for non-existant colls
/**
 * Tests for the db collection
 *
 * @tags: [
 *   requires_capped,
 *   requires_collstats,
 *   requires_fastcount,
 *   # In-memory data structures are not causally consistent.
 *   does_not_support_causal_consistency,
 * ]
 */
const testDb = db.getSiblingDB("test");
const collName = jsTestName();
const coll = testDb.getCollection(collName);
/*
 *  test drop
 */
coll.drop();
assert.eq(0, coll.find().length(), "1");

coll.save({a: 1});
assert.eq(1, coll.find().length(), "2");

coll.drop();
assert.eq(0, coll.find().length(), "3");

/*
 * test count
 */

assert.eq(0, coll.count(), "4");
coll.save({a: 1});
assert.eq(1, coll.count(), "5");
for (var i = 0; i < 100; i++) {
    coll.save({a: 1});
}
assert.eq(101, coll.count(), "6");
coll.drop();
assert.eq(0, coll.count(), "7");

/*
 * test validate
 */

coll.drop();
assert.eq(0, coll.count(), "8");

for (var i = 0; i < 100; i++) {
    coll.save({a: 1});
}

(function () {
    let validateResult = assert.commandWorked(coll.validate());
    // Extract validation results from mongos output if running in a sharded context.
    let isShardedNS = validateResult.hasOwnProperty("raw");

    if (isShardedNS) {
        // Sample mongos format:
        // {
        //   raw: {
        //     "localhost:30000": {
        //       "ns" : coll.getName(),
        //       ...
        //       "valid": true,
        //       ...
        //       "ok": 1
        //     }
        //   },
        //   "valid": true,
        //   ...
        //   "ok": 1
        // }

        let numFields = 0;
        let result = null;
        for (let field in validateResult.raw) {
            result = validateResult.raw[field];
            numFields++;
        }

        assert.eq(1, numFields);
        assert.neq(null, result);
        validateResult = result;
    }

    assert.eq(
        "test." + collName,
        validateResult.ns,
        "incorrect namespace in testDb.collection.validate() result: " + tojson(validateResult),
    );
    assert(validateResult.valid, "collection validation failed");
    assert.eq(100, validateResult.nrecords, "11");
})();

/*
 * test deleteIndex, deleteIndexes
 */

coll.drop();
assert.eq(0, coll.count(), "12");
coll.dropIndexes();
assert.eq(0, coll.getIndexes().length, "13");

coll.save({a: 10});
assert.eq(1, coll.getIndexes().length, "14");

coll.createIndex({a: 1});
coll.save({a: 10});

print(tojson(coll.getIndexes()));
assert.eq(2, coll.getIndexes().length, "15");

coll.dropIndex({a: 1});
assert.eq(1, coll.getIndexes().length, "16");

coll.save({a: 10});
coll.createIndex({a: 1});
coll.save({a: 10});

assert.eq(2, coll.getIndexes().length, "17");

coll.dropIndex("a_1");
assert.eq(1, coll.getIndexes().length, "18");

coll.save({a: 10, b: 11});
coll.createIndex({a: 1});
coll.createIndex({b: 1});
coll.save({a: 10, b: 12});

assert.eq(3, coll.getIndexes().length, "19");

coll.dropIndex({b: 1});
assert.eq(2, coll.getIndexes().length, "20");
coll.dropIndex({a: 1});
assert.eq(1, coll.getIndexes().length, "21");

coll.save({a: 10, b: 11});
coll.createIndex({a: 1});
coll.createIndex({b: 1});
coll.save({a: 10, b: 12});

assert.eq(3, coll.getIndexes().length, "22");

coll.dropIndexes();
assert.eq(1, coll.getIndexes().length, "23");

coll.find();

coll.drop();
assert.eq(0, coll.getIndexes().length, "24");

/*
 * stats()
 */

(function () {
    let t = testDb.apttest_dbcollection;

    // Non-existent collection.
    t.drop();
    let noCollStats = assert.commandWorked(
        t.stats(),
        "testDb.collection.stats() should work on non-existent collection",
    );
    assert.eq(0, noCollStats.size, "All properties should be 0 on nonexistant collections");
    assert.eq(0, noCollStats.count, "All properties should be 0 on nonexistant collections");
    assert.eq(0, noCollStats.storageSize, "All properties should be 0 on nonexistant collections");
    assert.eq(0, noCollStats.nindexes, "All properties should be 0 on nonexistant collections");
    assert.eq(0, noCollStats.totalIndexSize, "All properties should be 0 on nonexistant collections");
    assert.eq(0, noCollStats.totalSize, "All properties should be 0 on nonexistant collections");

    // scale - passed to stats() as sole numerical argument or part of an options object.
    t.drop();
    assert.commandWorked(testDb.createCollection(t.getName(), {capped: true, size: 10 * 1024 * 1024}));
    var collectionStats = assert.commandWorked(t.stats(1024 * 1024));
    assert.eq(
        10,
        collectionStats.maxSize,
        "testDb.collection.stats(scale) - capped collection size scaled incorrectly: " + tojson(collectionStats),
    );
    var collectionStats = assert.commandWorked(t.stats({scale: 1024 * 1024}));
    assert.eq(
        10,
        collectionStats.maxSize,
        "testDb.collection.stats({scale: N}) - capped collection size scaled incorrectly: " + tojson(collectionStats),
    );

    // Ensure that collStats can handle large values for 'scale'.
    var collectionStats = assert.commandWorked(t.stats(Number.MAX_VALUE));
    assert.eq(
        0,
        collectionStats.maxSize,
        "testDb.collection.stats(Number.MAX_VALUE) - capped collection size scaled incorrectly: " +
            tojson(collectionStats),
    );
    var collectionStats = assert.commandWorked(t.stats({scale: Number.MAX_VALUE}));
    assert.eq(
        0,
        collectionStats.maxSize,
        "testDb.collection.stats({scale: Number.MAX_VALUE}) - capped collection size scaled incorrectly: " +
            tojson(collectionStats),
    );

    // indexDetails - If true, includes 'indexDetails' field in results. Default: false.
    t.drop();
    t.save({a: 1});
    t.createIndex({a: 1});
    collectionStats = assert.commandWorked(t.stats());
    assert(
        !collectionStats.hasOwnProperty("indexDetails"),
        "unexpected indexDetails found in testDb.collection.stats() result: " + tojson(collectionStats),
    );
    collectionStats = assert.commandWorked(t.stats({indexDetails: false}));
    assert(
        !collectionStats.hasOwnProperty("indexDetails"),
        "unexpected indexDetails found in testDb.collection.stats({indexDetails: true}) result: " +
            tojson(collectionStats),
    );
    collectionStats = assert.commandWorked(t.stats({indexDetails: true}));
    assert(
        collectionStats.hasOwnProperty("indexDetails"),
        "indexDetails missing from testDb.collection.stats({indexDetails: true}) result: " + tojson(collectionStats),
    );

    // Returns index name.
    function getIndexName(indexKey) {
        let indexes = t.getIndexes().filter(function (doc) {
            return friendlyEqual(doc.key, indexKey);
        });
        assert.eq(1, indexes.length, tojson(indexKey) + " not found in getIndexes() result: " + tojson(t.getIndexes()));
        return indexes[0].name;
    }

    function checkIndexDetails(options, indexName) {
        let collectionStats = assert.commandWorked(t.stats(options));
        assert(
            collectionStats.hasOwnProperty("indexDetails"),
            "indexDetails missing from " +
                "testDb.collection.stats(" +
                tojson(options) +
                ") result: " +
                tojson(collectionStats),
        );
        // Currently, indexDetails is only supported with WiredTiger.
        let storageEngine = jsTest.options().storageEngine;
        if (storageEngine && storageEngine !== "wiredTiger") {
            return;
        }
        assert.eq(1, Object.keys(collectionStats.indexDetails).length, "indexDetails must have exactly one entry");
        assert(
            collectionStats.indexDetails[indexName],
            indexName + " missing from indexDetails: " + tojson(collectionStats.indexDetails),
        );
        assert.neq(
            0,
            Object.keys(collectionStats.indexDetails[indexName]).length,
            indexName + " exists in indexDetails but contains no information: " + tojson(collectionStats),
        );
    }

    // indexDetailsKey - show indexDetails results for this index key only.
    let indexKey = {a: 1};
    let indexName = getIndexName(indexKey);
    checkIndexDetails({indexDetails: true, indexDetailsKey: indexKey}, indexName);

    // indexDetailsName - show indexDetails results for this index name only.
    checkIndexDetails({indexDetails: true, indexDetailsName: indexName}, indexName);

    // Cannot specify both indexDetailsKey and indexDetailsName.
    let error = assert.throws(
        function () {
            t.stats({indexDetails: true, indexDetailsKey: indexKey, indexDetailsName: indexName});
        },
        [],
        "indexDetailsKey and indexDetailsName cannot be used at the same time",
    );
    assert.eq(
        Error,
        error.constructor,
        "testDb.collection.stats() failed when both indexDetailsKey and indexDetailsName " +
            "are used but with incorrect error type",
    );

    t.drop();
})();

/*
 * test testDb.collection.totalSize()
 */
(function () {
    let t = testDb.apitest_dbcollection;

    t.drop();
    let emptyStats = assert.commandWorked(t.stats());
    assert.eq(emptyStats.storageSize, 0);
    assert.eq(emptyStats.totalIndexSize, 0);

    assert.eq(0, t.storageSize(), "testDb.collection.storageSize() on empty collection should return 0");
    assert.eq(0, t.totalIndexSize(), "testDb.collection.totalIndexSize() on empty collection should return 0");
    assert.eq(0, t.totalSize(), "testDb.collection.totalSize() on empty collection should return 0");

    t.save({a: 1});
    let stats = assert.commandWorked(t.stats());
    assert.neq(
        undefined,
        t.storageSize(),
        "testDb.collection.storageSize() cannot be undefined on a non-empty collection",
    );
    assert.neq(
        undefined,
        t.totalIndexSize(),
        "testDb.collection.totalIndexSize() cannot be undefined on a non-empty collection",
    );

    t.drop();
})();
