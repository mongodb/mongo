/**
 * Tests that the server will startup normally when a collection has a missing id index.
 *
 * @tags: [requires_persistence]
 */
(function() {
"use strict";

load("jstests/libs/index_catalog_helpers.js");
load("jstests/libs/fail_point_util.js");

let conn = MongoRunner.runMongod();

const dbName = jsTestName();
const collName = "coll";

let testDB = conn.getDB(dbName);
let coll = testDB.getCollection(collName);

assert.commandWorked(testDB.adminCommand({configureFailPoint: "skipIdIndex", mode: "alwaysOn"}));
assert.commandWorked(testDB.createCollection(collName));

for (let i = 0; i < 1500; ++i) {
    assert.commandWorked(coll.insert({_id: i, a: 1}));
}

let indexSpec = IndexCatalogHelpers.findByName(coll.getIndexes(), "_id_");
assert.eq(indexSpec, null);

MongoRunner.stopMongod(conn);

// We should be able to successfully restart the server even though 'coll' is missing an id index.
conn = MongoRunner.runMongod({dbpath: conn.dbpath, noCleanData: true});
assert(conn);

// Check for the log message where we build missing id indexes in startup recovery.
checkLog.containsJson(conn, 4805002);

testDB = conn.getDB(dbName);
coll = testDB.getCollection(collName);

indexSpec = IndexCatalogHelpers.findByName(coll.getIndexes(), "_id_");
assert.neq(indexSpec, null);

MongoRunner.stopMongod(conn);
})();
