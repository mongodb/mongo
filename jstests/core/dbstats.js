// Confirms that the dbStats command returns expected content.
//
// @tags: [requires_dbstats]

(function() {
"use strict";

function serverIsMongos() {
    const res = db.runCommand("ismaster");
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
assert.writeOK(coll.insert(doc));

let dbStats = testDB.runCommand({dbStats: 1});
assert.commandWorked(dbStats);

assert.eq(1, dbStats.objects, tojson(dbStats));  // Includes testColl only
const dataSize = Object.bsonsize(doc);
assert.eq(dataSize, dbStats.avgObjSize, tojson(dbStats));
assert.eq(dataSize, dbStats.dataSize, tojson(dbStats));

// Index count will vary on mongoS if an additional index is needed to support sharding.
if (isMongoS) {
    assert(dbStats.hasOwnProperty("indexes"), tojson(dbStats));
} else {
    assert.eq(2, dbStats.indexes, tojson(dbStats));
}

assert(dbStats.hasOwnProperty("storageSize"), tojson(dbStats));
assert(dbStats.hasOwnProperty("totalSize"), tojson(dbStats));
assert(dbStats.hasOwnProperty("indexSize"), tojson(dbStats));

if (isUsingPersistentStorage) {
    assert(dbStats.hasOwnProperty("fsUsedSize"), tojson(dbStats));
    assert(dbStats.hasOwnProperty("fsTotalSize"), tojson(dbStats));
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
})();
