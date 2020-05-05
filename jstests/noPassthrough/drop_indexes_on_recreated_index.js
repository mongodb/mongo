/**
 * Verifies that the dropIndexes command does not invariant if it sees a similar index build
 * completed that it successfully aborted.
 */
(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/noPassthrough/libs/index_build.js");

const dbName = "test";
const collName = "dropIndexesOnRecreatedIndex";

const conn = MongoRunner.runMongod({});
assert(conn);

const db = conn.getDB(dbName);
assert.commandWorked(db.createCollection(collName));

const coll = db.getCollection(collName);
assert.commandWorked(coll.insert({a: 1}));

const indexSpec = {
    a: 1
};

IndexBuildTest.pauseIndexBuilds(conn);

// Start an index build on {a: 1}.
let awaitIndexBuild = IndexBuildTest.startIndexBuild(
    conn, coll.getFullName(), indexSpec, {}, [ErrorCodes.IndexBuildAborted]);
IndexBuildTest.waitForIndexBuildToStart(db, collName, "a_1");

const failPoint = "hangAfterAbortingIndexes";
let res = assert.commandWorked(db.adminCommand({configureFailPoint: failPoint, mode: "alwaysOn"}));
let timesEntered = res.count;

TestData.dbName = dbName;
TestData.collName = collName;
TestData.indexSpec = indexSpec;

// Abort {a: 1} while it is being built.
let awaitDropIndexes = startParallelShell(() => {
    assert.commandWorked(db.getSiblingDB(TestData.dbName)
                             .getCollection(TestData.collName)
                             .dropIndexes(TestData.indexSpec));
}, conn.port);

// Wait until {a: 1} is aborted, but before the dropIndexes command finishes.
awaitIndexBuild();
assert.commandWorked(conn.adminCommand({
    waitForFailPoint: failPoint,
    timesEntered: timesEntered + 1,
    maxTimeMS: kDefaultWaitForFailPointTimeout
}));

// Recreate {a: 1} and let it finish.
IndexBuildTest.resumeIndexBuilds(conn);
assert.commandWorked(coll.createIndex(indexSpec));

// Allow dropIndexes to finish. The dropIndexes should drop the newly recreated {a: 1} index.
assert.commandWorked(db.adminCommand({configureFailPoint: failPoint, mode: "off"}));
awaitDropIndexes();

// The collection should only have the _id index.
let indexes = coll.getIndexes();
assert.eq(1, indexes.length);

MongoRunner.stopMongod(conn);
}());
