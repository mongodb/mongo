/**
 *   Tests for the db collection
 */

/*
 *  test drop
 */
db.getCollection("test_db").drop();
assert.eq(0, db.getCollection("test_db").find().length(), "1");

db.getCollection("test_db").save({a: 1});
assert.eq(1, db.getCollection("test_db").find().length(), "2");

db.getCollection("test_db").drop();
assert.eq(0, db.getCollection("test_db").find().length(), "3");

/*
 * test count
 */

assert.eq(0, db.getCollection("test_db").count(), "4");
db.getCollection("test_db").save({a: 1});
assert.eq(1, db.getCollection("test_db").count(), "5");
for (i = 0; i < 100; i++) {
    db.getCollection("test_db").save({a: 1});
}
assert.eq(101, db.getCollection("test_db").count(), "6");
db.getCollection("test_db").drop();
assert.eq(0, db.getCollection("test_db").count(), "7");

/*
 * test validate
 */

db.getCollection("test_db").drop();
assert.eq(0, db.getCollection("test_db").count(), "8");

for (i = 0; i < 100; i++) {
    db.getCollection("test_db").save({a: 1});
}

(function() {
    var validateResult = assert.commandWorked(db.getCollection("test_db").validate());
    // Extract validation results from mongos output if running in a sharded context.
    var isShardedNS = validateResult.hasOwnProperty('raw');

    if (isShardedNS) {
        // Sample mongos format:
        // {
        //   raw: {
        //     "localhost:30000": {
        //       "ns" : "test.test_db",
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

        var numFields = 0;
        var result = null;
        for (var field in validateResult.raw) {
            result = validateResult.raw[field];
            numFields++;
        }

        assert.eq(1, numFields);
        assert.neq(null, result);
        validateResult = result;
    }

    assert.eq('test.test_db',
              validateResult.ns,
              'incorrect namespace in db.collection.validate() result: ' + tojson(validateResult));
    assert(validateResult.valid, 'collection validation failed');
    assert.eq(100, validateResult.nrecords, 11);
}());

/*
 * test deleteIndex, deleteIndexes
 */

db.getCollection("test_db").drop();
assert.eq(0, db.getCollection("test_db").count(), "12");
db.getCollection("test_db").dropIndexes();
assert.eq(0, db.getCollection("test_db").getIndexes().length, "13");

db.getCollection("test_db").save({a: 10});
assert.eq(1, db.getCollection("test_db").getIndexes().length, "14");

db.getCollection("test_db").ensureIndex({a: 1});
db.getCollection("test_db").save({a: 10});

print(tojson(db.getCollection("test_db").getIndexes()));
assert.eq(2, db.getCollection("test_db").getIndexes().length, "15");

db.getCollection("test_db").dropIndex({a: 1});
assert.eq(1, db.getCollection("test_db").getIndexes().length, "16");

db.getCollection("test_db").save({a: 10});
db.getCollection("test_db").ensureIndex({a: 1});
db.getCollection("test_db").save({a: 10});

assert.eq(2, db.getCollection("test_db").getIndexes().length, "17");

db.getCollection("test_db").dropIndex("a_1");
assert.eq(1, db.getCollection("test_db").getIndexes().length, "18");

db.getCollection("test_db").save({a: 10, b: 11});
db.getCollection("test_db").ensureIndex({a: 1});
db.getCollection("test_db").ensureIndex({b: 1});
db.getCollection("test_db").save({a: 10, b: 12});

assert.eq(3, db.getCollection("test_db").getIndexes().length, "19");

db.getCollection("test_db").dropIndex({b: 1});
assert.eq(2, db.getCollection("test_db").getIndexes().length, "20");
db.getCollection("test_db").dropIndex({a: 1});
assert.eq(1, db.getCollection("test_db").getIndexes().length, "21");

db.getCollection("test_db").save({a: 10, b: 11});
db.getCollection("test_db").ensureIndex({a: 1});
db.getCollection("test_db").ensureIndex({b: 1});
db.getCollection("test_db").save({a: 10, b: 12});

assert.eq(3, db.getCollection("test_db").getIndexes().length, "22");

db.getCollection("test_db").dropIndexes();
assert.eq(1, db.getCollection("test_db").getIndexes().length, "23");

db.getCollection("test_db").find();

db.getCollection("test_db").drop();
assert.eq(0, db.getCollection("test_db").getIndexes().length, "24");

/*
 * stats()
 */

(function() {
    var t = db.apttest_dbcollection;

    // Non-existent collection.
    t.drop();
    assert.commandFailed(t.stats(), 'db.collection.stats() should fail on non-existent collection');

    // scale - passed to stats() as sole numerical argument or part of an options object.
    t.drop();
    assert.commandWorked(db.createCollection(t.getName(), {capped: true, size: 10 * 1024 * 1024}));
    var collectionStats = assert.commandWorked(t.stats(1024 * 1024));
    assert.eq(10,
              collectionStats.maxSize,
              'db.collection.stats(scale) - capped collection size scaled incorrectly: ' +
                  tojson(collectionStats));
    var collectionStats = assert.commandWorked(t.stats({scale: 1024 * 1024}));
    assert.eq(10,
              collectionStats.maxSize,
              'db.collection.stats({scale: N}) - capped collection size scaled incorrectly: ' +
                  tojson(collectionStats));

    // indexDetails - If true, includes 'indexDetails' field in results. Default: false.
    t.drop();
    t.save({a: 1});
    t.ensureIndex({a: 1});
    collectionStats = assert.commandWorked(t.stats());
    assert(!collectionStats.hasOwnProperty('indexDetails'),
           'unexpected indexDetails found in db.collection.stats() result: ' +
               tojson(collectionStats));
    collectionStats = assert.commandWorked(t.stats({indexDetails: false}));
    assert(!collectionStats.hasOwnProperty('indexDetails'),
           'unexpected indexDetails found in db.collection.stats({indexDetails: true}) result: ' +
               tojson(collectionStats));
    collectionStats = assert.commandWorked(t.stats({indexDetails: true}));
    assert(collectionStats.hasOwnProperty('indexDetails'),
           'indexDetails missing from db.collection.stats({indexDetails: true}) result: ' +
               tojson(collectionStats));

    // Returns index name.
    function getIndexName(indexKey) {
        var indexes = t.getIndexes().filter(function(doc) {
            return friendlyEqual(doc.key, indexKey);
        });
        assert.eq(
            1,
            indexes.length,
            tojson(indexKey) + ' not found in getIndexes() result: ' + tojson(t.getIndexes()));
        return indexes[0].name;
    }

    function checkIndexDetails(options, indexName) {
        var collectionStats = assert.commandWorked(t.stats(options));
        assert(collectionStats.hasOwnProperty('indexDetails'),
               'indexDetails missing from ' +
                   'db.collection.stats(' + tojson(options) + ') result: ' +
                   tojson(collectionStats));
        // Currently, indexDetails is only supported with WiredTiger.
        var storageEngine = jsTest.options().storageEngine;
        if (storageEngine && storageEngine !== 'wiredTiger') {
            return;
        }
        assert.eq(1,
                  Object.keys(collectionStats.indexDetails).length,
                  'indexDetails must have exactly one entry');
        assert(collectionStats.indexDetails[indexName],
               indexName + ' missing from indexDetails: ' + tojson(collectionStats.indexDetails));
        assert.neq(0,
                   Object.keys(collectionStats.indexDetails[indexName]).length,
                   indexName + ' exists in indexDetails but contains no information: ' +
                       tojson(collectionStats));
    }

    // indexDetailsKey - show indexDetails results for this index key only.
    var indexKey = {a: 1};
    var indexName = getIndexName(indexKey);
    checkIndexDetails({indexDetails: true, indexDetailsKey: indexKey}, indexName);

    // indexDetailsName - show indexDetails results for this index name only.
    checkIndexDetails({indexDetails: true, indexDetailsName: indexName}, indexName);

    // Cannot specify both indexDetailsKey and indexDetailsName.
    var error = assert.throws(function() {
        t.stats({indexDetails: true, indexDetailsKey: indexKey, indexDetailsName: indexName});
    }, [], 'indexDetailsKey and indexDetailsName cannot be used at the same time');
    assert.eq(Error,
              error.constructor,
              'db.collection.stats() failed when both indexDetailsKey and indexDetailsName ' +
                  'are used but with incorrect error type');

    t.drop();
}());

/*
 * test db.collection.totalSize()
 */
(function() {
    'use strict';

    var t = db.apitest_dbcollection;

    t.drop();
    var failedStats = assert.commandFailed(t.stats());
    assert.eq(failedStats.storageSize, t.storageSize());
    assert.eq(undefined,
              t.storageSize(),
              'db.collection.storageSize() on empty collection should return undefined');
    assert.eq(failedStats.totalIndexSize, t.totalIndexSize());
    assert.eq(undefined,
              t.totalIndexSize(),
              'db.collection.totalIndexSize() on empty collection should return undefined');
    assert.eq(undefined,
              t.totalSize(),
              'db.collection.totalSize() on empty collection should return undefined');

    t.save({a: 1});
    var stats = assert.commandWorked(t.stats());
    assert.neq(undefined,
               t.storageSize(),
               'db.collection.storageSize() cannot be undefined on a non-empty collection');
    assert.neq(undefined,
               t.totalIndexSize(),
               'db.collection.totalIndexSize() cannot be undefined on a non-empty collection');

    if (db.isMaster().msg !== 'isdbgrid' && db.serverStatus().storageEngine.name === 'mmapv1') {
        // Only in MMAPv1 do we guarantee that storageSize only changes when you write to a
        // collection.
        assert.eq(stats.storageSize, t.storageSize());
        assert.eq(stats.totalIndexSize, t.totalIndexSize());
        assert.eq(t.storageSize() + t.totalIndexSize(),
                  t.totalSize(),
                  'incorrect db.collection.totalSize() on a non-empty collection');
    }

    t.drop();
}());
