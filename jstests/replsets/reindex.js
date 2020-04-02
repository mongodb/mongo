/**
 * Test that the reindex command only runs on a node in standalone mode. First it will make sure
 * that the command can't be run on a primary or a secondary. Then it will make sure that the
 * reindex command can be successfully run on a standalone node.
 */

(function() {
"use strict";

jsTestLog("Testing that the reindex command cannot be run on a primary or secondary");

const replTest = new ReplSetTest({name: 'reindexTest', nodes: 2});
replTest.startSet();
replTest.initiate();

const primary = replTest.getPrimary();
const secondary = replTest.getSecondary();

const dbName = "test";
const collName = "reindex";
const primaryDB = primary.getDB(dbName);
const primaryColl = primaryDB.getCollection(collName);
const secondaryDB = secondary.getDB(dbName);
const secondaryColl = secondaryDB.getCollection(collName);

assert.commandWorked(primaryColl.insert({a: 1000}));
assert.commandWorked(primaryColl.ensureIndex({a: 1}));

replTest.awaitReplication();
replTest.waitForAllIndexBuildsToFinish(dbName, collName);

assert.eq(2,
          primaryColl.getIndexes().length,
          "Primary didn't have expected number of indexes before reindex");
assert.eq(2,
          secondaryColl.getIndexes().length,
          "Secondary didn't have expected number of indexes before reindex");

assert.commandFailedWithCode(primaryColl.reIndex(), ErrorCodes.IllegalOperation);
assert.commandFailedWithCode(secondaryColl.reIndex(), ErrorCodes.IllegalOperation);

assert.eq(2,
          primaryColl.getIndexes().length,
          "Primary didn't have expected number of indexes after failed reindex");
assert.eq(2,
          secondaryColl.getIndexes().length,
          "Secondary didn't have expected number of indexes after failed reindex");

replTest.stopSet();

jsTestLog("Testing that the reindex command can successfully be run on a standalone node");

const standalone = MongoRunner.runMongod({});
assert.neq(null, standalone, "mongod failed to start.");

const testDB = standalone.getDB(dbName);
const testColl = testDB.getCollection(collName);

assert.commandWorked(testColl.insert({a: 1000}));
assert.commandWorked(testColl.ensureIndex({a: 1}));
assert.eq(2, testColl.getIndexes().length, "Standalone didn't have proper indexes before reindex");

assert.commandWorked(testColl.reIndex());

assert.eq(2, testColl.getIndexes().length, "Standalone didn't have proper indexes after reindex");

MongoRunner.stopMongod(standalone);
})();
