// Tests user queries over the config servers.
// @tags: [
//    requires_scripting,
// ]

import {ShardingTest} from "jstests/libs/shardingtest.js";
import {findChunksUtil} from "jstests/sharding/libs/find_chunks_util.js";

let getListCollectionsCursor = function (database, options, subsequentBatchSize) {
    return new DBCommandCursor(database, database.runCommand("listCollections", options), subsequentBatchSize);
};

let getListIndexesCursor = function (coll, options, subsequentBatchSize) {
    return new DBCommandCursor(coll.getDB(), coll.runCommand("listIndexes", options), subsequentBatchSize);
};

let arrayGetNames = function (array) {
    return array.map(function (spec) {
        return spec.name;
    });
};

let cursorGetCollectionNames = function (cursor) {
    return arrayGetNames(cursor.toArray());
};

let sortArrayByName = function (array) {
    return array.sort(function (a, b) {
        return a.name > b.name;
    });
};

let sortArrayById = function (array) {
    return array.sort(function (a, b) {
        return a._id > b._id;
    });
};

let dropCollectionIfExists = function (coll) {
    try {
        coll.drop();
    } catch (err) {
        assert.eq(err.code, ErrorCodes.NamespaceNotFound);
    }
};

/**
 * Sets up the test database with with several sharded collections.
 *
 * @return The list of collection namespaces that were added to the test database.
 */
let setupTestCollections = function (st) {
    // testKeys and testCollNames are parallel arrays, testKeys contains the shard key of the
    // corresponding collection whose name is in testCollNames.
    let testCollNames = ["4a1", "1a12", "3a1b1", "2a1b1c1", "b1", "b1c1", "d1"];
    let testKeys = [{a: 1}, {a: 1}, {a: 1, b: 1}, {a: 1, b: 1, c: 1}, {b: 1}, {b: 1, c: 1}, {d: 1}];
    let testDB = st.s.getDB("test");

    assert.commandWorked(st.s.adminCommand({enablesharding: testDB.getName()}));
    let testNamespaces = testCollNames.map(function (e) {
        return testDB.getName() + "." + e;
    });
    for (let i = 0; i < testKeys.length; i++) {
        assert.commandWorked(st.s.adminCommand({shardcollection: testNamespaces[i], key: testKeys[i]}));
    }

    return testNamespaces;
};

/**
 * Test that a list collections query works on the config database. This test cannot detect
 * whether list collections lists extra collections.
 */
let testListConfigCollections = function (st) {
    // This test depends on all the collections in the configCollList being in the config
    // database.
    let configCollList = ["chunks", "collections", "databases", "shards", "tags", "version"];
    let configDB = st.s.getDB("config");
    let userAddedColl = configDB.userAddedColl;
    let cursor;
    let cursorArray;

    // Dropping collections under config is not allowed via mongos.
    let configPrimary = st.configRS.getPrimary();
    dropCollectionIfExists(configPrimary.getDB("config").userAddedColl);
    configDB.createCollection(userAddedColl.getName());
    configCollList.push(userAddedColl.getName());

    // wait for config.mongos to be created by RouterUptimeReporter
    assert.soon(() => configDB.mongos.exists());

    cursor = getListCollectionsCursor(configDB);
    cursorArray = cursorGetCollectionNames(cursor);
    for (let i = 0; i < configCollList.length; i++) {
        assert(cursorArray.indexOf(configCollList[i]) > -1, "Missing " + configCollList[i]);
    }

    cursor = getListCollectionsCursor(configDB, {cursor: {batchSize: 1}}, 1);
    assert.eq(cursor.objsLeftInBatch(), 1);
    assert(cursorArray.indexOf(cursor.next().name) > -1);
    assert(cursor.hasNext());
    assert.eq(cursor.objsLeftInBatch(), 1);
    assert(cursorArray.indexOf(cursor.next().name) > -1);
};

/**
 * Test that a list indexes query works on the chunks collection of the config database.
 */
let testListConfigChunksIndexes = function (st) {
    // This test depends on all the indexes in the configChunksIndexes being the exact indexes
    // in the config chunks collection.
    let configDB = st.s.getDB("config");
    let expectedConfigChunksIndexes = [
        "_id_",
        "uuid_1_lastmod_1",
        "uuid_1_min_1",
        "uuid_1_shard_1_min_1",
        "uuid_1_shard_1_onCurrentShardSince_1",
    ];
    const foundIndexesArray = getListIndexesCursor(configDB.chunks).toArray();

    assert.eq(foundIndexesArray.length, expectedConfigChunksIndexes.length);
    assert.eq(arrayGetNames(sortArrayByName(foundIndexesArray)), expectedConfigChunksIndexes);
};

/**
 * Test queries over the collections collection of the config database.
 */
let queryConfigCollections = function (st, testNamespaces) {
    let configDB = st.s.getDB("config");
    let cursor;

    // Find query.
    cursor = configDB.collections.find({"key.a": 1}, {"key.a": 1, "key.c": 1}).sort({"_id": 1}).batchSize(2);
    assert.eq(cursor.objsLeftInBatch(), 2);
    assert.eq(cursor.next(), {_id: testNamespaces[1], key: {a: 1}});
    assert.eq(cursor.next(), {_id: testNamespaces[3], key: {a: 1, c: 1}});
    assert(cursor.hasNext());
    assert.eq(cursor.objsLeftInBatch(), 2);
    assert.eq(cursor.next(), {_id: testNamespaces[2], key: {a: 1}});
    assert.eq(cursor.next(), {_id: testNamespaces[0], key: {a: 1}});
    assert(!cursor.hasNext());

    // Aggregate query.
    cursor = configDB.collections.aggregate(
        [{$match: {"key.b": 1}}, {$sort: {"_id": 1}}, {$project: {"keyb": "$key.b", "keyc": "$key.c"}}],
        {cursor: {batchSize: 2}},
    );
    assert.eq(cursor.objsLeftInBatch(), 2);
    assert.eq(cursor.next(), {_id: testNamespaces[3], keyb: 1, keyc: 1});
    assert.eq(cursor.next(), {_id: testNamespaces[2], keyb: 1});
    assert(cursor.hasNext());
    assert.eq(cursor.objsLeftInBatch(), 2);
    assert.eq(cursor.next(), {_id: testNamespaces[4], keyb: 1});
    assert.eq(cursor.next(), {_id: testNamespaces[5], keyb: 1, keyc: 1});
    assert(!cursor.hasNext());
};

/**
 * Test queries over the chunks collection of the config database.
 */
let queryConfigChunks = function (st) {
    let configDB = st.s.getDB("config");
    let testDB = st.s.getDB("test2");
    let testColl = testDB.testColl;
    let testCollData = [{e: 1}, {e: 3}, {e: 4}, {e: 5}, {e: 7}, {e: 9}, {e: 10}, {e: 12}];
    let cursor;
    let result;

    // Get shard names.
    cursor = configDB.shards.find().sort({_id: 1});
    let shard1 = cursor.next()._id;
    let shard2 = cursor.next()._id;
    assert(!cursor.hasNext());
    assert.commandWorked(st.s.adminCommand({enablesharding: testDB.getName(), primaryShard: shard1}));

    // Setup.
    assert.commandWorked(st.s.adminCommand({shardcollection: testColl.getFullName(), key: {e: 1}}));
    for (let i = 0; i < testCollData.length; i++) {
        assert.commandWorked(testColl.insert(testCollData[i]));
    }
    assert.commandWorked(st.s.adminCommand({split: testColl.getFullName(), middle: {e: 2}}));
    assert.commandWorked(st.s.adminCommand({split: testColl.getFullName(), middle: {e: 6}}));
    assert.commandWorked(st.s.adminCommand({split: testColl.getFullName(), middle: {e: 8}}));
    assert.commandWorked(st.s.adminCommand({split: testColl.getFullName(), middle: {e: 11}}));
    assert.commandWorked(st.s.adminCommand({movechunk: testColl.getFullName(), find: {e: 1}, to: shard2}));
    assert.commandWorked(st.s.adminCommand({movechunk: testColl.getFullName(), find: {e: 9}, to: shard2}));
    assert.commandWorked(st.s.adminCommand({movechunk: testColl.getFullName(), find: {e: 12}, to: shard2}));

    // Find query.
    cursor = findChunksUtil
        .findChunksByNs(configDB, testColl.getFullName(), null, {_id: 0, min: 1, max: 1, shard: 1})
        .sort({"min.e": 1});
    assert.eq(cursor.next(), {min: {e: {"$minKey": 1}}, "max": {"e": 2}, shard: shard2});
    assert.eq(cursor.next(), {min: {e: 2}, max: {e: 6}, shard: shard1});
    assert.eq(cursor.next(), {min: {e: 6}, max: {e: 8}, shard: shard1});
    assert.eq(cursor.next(), {min: {e: 8}, max: {e: 11}, shard: shard2});
    assert.eq(cursor.next(), {min: {e: 11}, max: {e: {"$maxKey": 1}}, shard: shard2});
    assert(!cursor.hasNext());

    // Count query with filter.
    assert.eq(findChunksUtil.countChunksForNs(configDB, testColl.getFullName()), 5);

    // Distinct query.
    assert.eq(configDB.chunks.distinct("shard").sort(), [shard1, shard2]);

    // Map reduce query.
    const coll = configDB.collections.findOne({_id: testColl.getFullName()});
    /* eslint-disable */
    var mapFunction = function () {
        if (xx.timestamp) {
            if (this.uuid.toString() == xx.uuid.toString()) {
                emit(this.shard, 1);
            }
        } else {
            if (this.ns == "test2.testColl") {
                emit(this.shard, 1);
            }
        }
    };
    /* eslint-enable */
    let reduceFunction = function (key, values) {
        // We may be re-reducing values that have already been partially reduced. In that case, we
        // expect to see an object like {chunks: <count>} in the array of input values.
        const numValues = values.reduce(function (acc, currentValue) {
            if (typeof currentValue === "object") {
                return acc + currentValue.chunks;
            } else {
                return acc + 1;
            }
        }, 0);
        return {chunks: numValues};
    };
    result = configDB.chunks.mapReduce(mapFunction, reduceFunction, {
        out: {inline: 1},
        scope: {xx: {timestamp: coll.timestamp, uuid: coll.uuid}},
    });
    assert.eq(result.ok, 1);
    assert.eq(sortArrayById(result.results), [
        {_id: shard1, value: {chunks: 2}},
        {_id: shard2, value: {chunks: 3}},
    ]);
};

/**
 * Test queries over a user created collection of an arbitrary database on the config servers.
 */
let queryUserCreated = function (database) {
    let userColl = database.userColl;
    let userCollData = [
        {_id: 1, g: 1, c: 4, s: "c", u: [1, 2]},
        {_id: 2, g: 1, c: 5, s: "b", u: [1]},
        {_id: 3, g: 2, c: 16, s: "g", u: [3]},
        {_id: 4, g: 2, c: 1, s: "a", u: [2, 4]},
        {_id: 5, g: 2, c: 18, s: "d", u: [3]},
        {_id: 6, g: 3, c: 11, s: "e", u: [2, 3]},
        {_id: 7, g: 3, c: 2, s: "f", u: [1]},
    ];
    let userCollIndexes = ["_id_", "s_1"];
    let cursor;
    let cursorArray;
    let result;

    // Dropping collections under config is not allowed via mongos.
    let configPrimary = st.configRS.getPrimary();
    dropCollectionIfExists(configPrimary.getDB(database.getName()).userColl);
    for (let i = 0; i < userCollData.length; i++) {
        assert.commandWorked(userColl.insert(userCollData[i]));
    }
    assert.commandWorked(userColl.createIndex({s: 1}));

    // List indexes.
    cursorArray = [];
    cursor = getListIndexesCursor(userColl, {cursor: {batchSize: 1}}, 1);
    assert.eq(cursor.objsLeftInBatch(), 1);
    cursorArray.push(cursor.next());
    assert(cursor.hasNext());
    assert.eq(cursor.objsLeftInBatch(), 1);
    cursorArray.push(cursor.next());
    assert(!cursor.hasNext());
    assert.eq(arrayGetNames(sortArrayByName(cursorArray)), userCollIndexes);

    // Find query.
    cursor = userColl
        .find({g: {$gte: 2}}, {_id: 0, c: 1})
        .sort({s: 1})
        .batchSize(2);
    assert.eq(cursor.objsLeftInBatch(), 2);
    assert.eq(cursor.next(), {c: 1});
    assert.eq(cursor.objsLeftInBatch(), 1);
    assert.eq(cursor.next(), {c: 18});
    assert(cursor.hasNext());
    assert.eq(cursor.objsLeftInBatch(), 2);
    assert.eq(cursor.next(), {c: 11});
    assert.eq(cursor.objsLeftInBatch(), 1);
    assert.eq(cursor.next(), {c: 2});
    assert(cursor.hasNext());
    assert.eq(cursor.objsLeftInBatch(), 1);
    assert.eq(cursor.next(), {c: 16});
    assert(!cursor.hasNext());

    // Aggregate query.
    cursor = userColl.aggregate(
        [{$match: {c: {$gt: 1}}}, {$unwind: "$u"}, {$group: {_id: "$u", sum: {$sum: "$c"}}}, {$sort: {_id: 1}}],
        {cursor: {batchSize: 2}},
    );
    assert.eq(cursor.objsLeftInBatch(), 2);
    assert.eq(cursor.next(), {_id: 1, sum: 11});
    assert.eq(cursor.next(), {_id: 2, sum: 15});
    assert(cursor.hasNext());
    assert.eq(cursor.objsLeftInBatch(), 1);
    assert.eq(cursor.next(), {_id: 3, sum: 45});
    assert(!cursor.hasNext());

    // Count query without filter.
    assert.eq(userColl.count(), userCollData.length);

    // Count query with filter.
    assert.eq(userColl.count({g: 2}), 3);

    // Distinct query.
    assert.eq(userColl.distinct("g").sort(), [1, 2, 3]);

    // Map reduce query.
    let mapFunction = function () {
        emit(this.g, 1);
    };
    let reduceFunction = function (key, values) {
        // We may be re-reducing values that have already been partially reduced. In that case, we
        // expect to see an object like {count: <count>} in the array of input values.
        const numValues = values.reduce(function (acc, currentValue) {
            if (typeof currentValue === "object") {
                return acc + currentValue.count;
            } else {
                return acc + 1;
            }
        }, 0);
        return {count: numValues};
    };
    result = userColl.mapReduce(mapFunction, reduceFunction, {out: {inline: 1}});
    assert.eq(result.ok, 1);
    assert.eq(sortArrayById(result.results), [
        {_id: 1, value: {count: 2}},
        {_id: 2, value: {count: 3}},
        {_id: 3, value: {count: 2}},
    ]);
};

var st = new ShardingTest({shards: 2, mongos: 1});
let testNamespaces = setupTestCollections(st);
let configDB = st.s.getDB("config");
let adminDB = st.s.getDB("admin");

testListConfigCollections(st);
testListConfigChunksIndexes(st);
queryConfigCollections(st, testNamespaces);
queryConfigChunks(st);
queryUserCreated(configDB);
queryUserCreated(adminDB);
st.stop();
