// Confirms that the dbStats command returns expected content.
//
// @tags: [
//   # Asserts on the total number of indexes in dbStats.
//   assumes_no_implicit_index_creation,
//   requires_dbstats,
//   requires_fcv_53,
//   # The `dbstats` command builds in-memory structures that are not causally consistent.
//   does_not_support_causal_consistency,
// ]
import {
    ClusteredCollectionUtil
} from "jstests/libs/clustered_collections/clustered_collection_util.js";

function serverIsMongos() {
    const res = db.runCommand("hello");
    assert.commandWorked(res);
    return res.msg === "isdbgrid";
}

function serverUsingPersistentStorage() {
    const res = db.runCommand("serverStatus");
    assert.commandWorked(res);
    return res.storageEngine.persistent === true;
}

const isMongoS = serverIsMongos();
const isUsingPersistentStorage = !isMongoS && serverUsingPersistentStorage();

let testDB = db.getSiblingDB("dbstats_js");
assert.commandWorked(testDB.dropDatabase());

let coll = testDB["testColl"];
assert.commandWorked(coll.createIndex({x: 1}));
const doc = {
    _id: 1,
    x: 1
};
assert.commandWorked(coll.insert(doc));

let dbStats = testDB.runCommand({dbStats: 1, freeStorage: 1});
assert.commandWorked(dbStats);

assert.eq(1, dbStats.objects, tojson(dbStats));  // Includes testColl only
const dataSize = Object.bsonsize(doc);
assert.eq(dataSize, dbStats.avgObjSize, tojson(dbStats));
assert.eq(dataSize, dbStats.dataSize, tojson(dbStats));

// Index count will vary on mongoS if an additional index is needed to support sharding.
if (isMongoS) {
    assert(dbStats.hasOwnProperty("indexes"), tojson(dbStats));
} else if (ClusteredCollectionUtil.areAllCollectionsClustered(db.getMongo())) {
    // A clustered collection has no actual index on _id.
    assert.eq(1, dbStats.indexes, tojson(dbStats));
} else {
    assert.eq(2, dbStats.indexes, tojson(dbStats));
}

assert(dbStats.hasOwnProperty("storageSize"), tojson(dbStats));
assert(dbStats.hasOwnProperty("totalSize"), tojson(dbStats));
assert(dbStats.hasOwnProperty("indexSize"), tojson(dbStats));

if (isUsingPersistentStorage) {
    assert(dbStats.hasOwnProperty("freeStorageSize"), tojson(dbStats));
    assert(dbStats.hasOwnProperty("indexFreeStorageSize"), tojson(dbStats));
    assert(dbStats.hasOwnProperty("totalFreeStorageSize"), tojson(dbStats));
    assert.eq(dbStats.freeStorageSize + dbStats.indexFreeStorageSize, dbStats.totalFreeStorageSize);

    assert(dbStats.hasOwnProperty("fsUsedSize"), tojson(dbStats));
    assert(dbStats.hasOwnProperty("fsTotalSize"), tojson(dbStats));

    // Make sure free storage size is not included by default
    const defaultStats = testDB.runCommand({dbStats: 1});
    assert.commandWorked(defaultStats);
    assert(!defaultStats.hasOwnProperty("freeStorageSize"), tojson(defaultStats));
    assert(!defaultStats.hasOwnProperty("indexFreeStorageSize"), tojson(defaultStats));
    assert(!defaultStats.hasOwnProperty("totalFreeStorageSize"), tojson(defaultStats));
}

// Confirm collection and view counts on mongoD
if (!isMongoS) {
    assert.eq(testDB.getName(), dbStats.db, tojson(dbStats));

    // We wait to add a view until this point as it allows more exact testing of avgObjSize for
    // WiredTiger above. Having more than 1 document would require floating point comparison.
    assert.commandWorked(testDB.createView("testView", "testColl", []));

    dbStats = testDB.runCommand({dbStats: 1});
    assert.commandWorked(dbStats);

    assert.eq(2, dbStats.collections, tojson(dbStats));  // testColl + system.views
    assert.eq(1, dbStats.views, tojson(dbStats));
}

// Check that the output for non-existing database and  the output for empty database
// have the same fields.
const testEmptyAndNonExistingDB = db.getSiblingDB(jsTestName() + "_non_existing_and_empty");
testEmptyAndNonExistingDB.dropDatabase();

const statsNonExistingDB = testEmptyAndNonExistingDB.runCommand({dbStats: 1, freeStorage: 1});
testEmptyAndNonExistingDB.runCommand({create: "test_empty_collection"});

const statsEmptyDB = testEmptyAndNonExistingDB.runCommand({dbStats: 1, freeStorage: 1});
assert.sameMembers(Object.keys(statsNonExistingDB),
                   Object.keys(statsEmptyDB),
                   "dbStats for non-existing and empty dbs should return the same fields");