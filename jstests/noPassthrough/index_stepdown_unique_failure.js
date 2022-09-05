/**
 * Confirms that unique index builds fail after a primary steps down during the collection
 * scan phase. The new primary will discover a duplicate key violation and abort the build.
 * @tags: [
 *   requires_replication,
 * ]
 */
(function() {
"use strict";

load('jstests/noPassthrough/libs/index_build.js');

const rst = new ReplSetTest({
    nodes: [
        {},
        {},
    ]
});
const nodes = rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const dbName = 'test';
const collName = 'coll';
const testDB = primary.getDB(dbName);
const coll = testDB.getCollection(collName);

assert.commandWorked(coll.insert({a: 1}));

IndexBuildTest.pauseIndexBuilds(primary);
IndexBuildTest.pauseIndexBuilds(rst.getSecondary());

const createIdx =
    IndexBuildTest.startIndexBuild(primary, coll.getFullName(), {a: 1}, {unique: true});

// When the index build starts, find its op id.
const opId = IndexBuildTest.waitForIndexBuildToScanCollection(testDB, coll.getName(), 'a_1');

IndexBuildTest.assertIndexBuildCurrentOpContents(testDB, opId);

// Step down the primary.
assert.commandWorked(primary.adminCommand({replSetStepDown: 60, force: true}));

// Confirm failover.
const newPrimary = rst.getPrimary();
assert.neq(primary.port, newPrimary.port);

// Insert a duplicate, which should succeed. This will cause the index build to fail later on.
assert.commandWorked(newPrimary.getDB(dbName).getCollection(collName).insert({a: 1}));

// Wait for the index build to stop.
IndexBuildTest.resumeIndexBuilds(primary);
IndexBuildTest.resumeIndexBuilds(newPrimary);
IndexBuildTest.waitForIndexBuildToStop(testDB);
IndexBuildTest.waitForIndexBuildToStop(newPrimary.getDB(dbName));

const exitCode = createIdx({checkExitSuccess: false});
assert.neq(0, exitCode, 'expected shell to exit abnormally due to index build being interrupted');

// The index build should have failed because of the duplicate key violation.
rst.awaitReplication();
const primaryColl = rst.getPrimary().getDB(dbName).getCollection(collName);
let res = assert.commandWorked(primaryColl.validate());
assert(res.valid, 'expected validation to succeed: ' + tojson(res));

const secondaryColl = rst.getSecondary().getDB(dbName).getCollection(collName);
res = assert.commandWorked(secondaryColl.validate());
assert(res.valid, 'expected validation to succeed: ' + tojson(res));

IndexBuildTest.assertIndexes(primaryColl, 1, ['_id_']);
IndexBuildTest.assertIndexes(secondaryColl, 1, ['_id_']);

rst.stopSet();
})();
