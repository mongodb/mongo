/*
 * Tests that 'recoverFromOplogAsStandalone' relaxes index constraints. This test is
 * non-deterministic. If there were a bug, it would still succeed with low probability, but should
 * never fail without a bug.
 *
 * This test only makes sense for storage engines that support recover to stable timestamp.
 * @tags: [requires_persistence, requires_replication,
 * requires_majority_read_concern,
 * # Restarting as a standalone is not supported in multiversion tests.
 * multiversion_incompatible]
 */

(function() {
"use strict";
load("jstests/replsets/rslib.js");
load("jstests/libs/write_concern_util.js");

const name = jsTestName();
const dbName = name;
const collName = 'coll';
const logLevel = tojson({storage: {recovery: 2}, replication: 3});

const rst = new ReplSetTest({
    nodes: 1,
});

function getColl(conn) {
    return conn.getDB(dbName)[collName];
}

jsTestLog("Initiating as a replica set.");
rst.startSet();
rst.initiate();
let node = rst.getPrimary();

assert.commandWorked(getColl(node).insert({_id: 1}, {writeConcern: {w: 1, j: 1}}));
assert.commandWorked(getColl(node).createIndex({x: 1}, {unique: true}));

jsTestLog("Running inserts and removes");
const start = (new Date()).getTime();
const waitTimeMillis = 5 * 1000;
const baseNum = 10;
let iter = 2;
Random.setRandomSeed();
while (((new Date()).getTime() - start) < waitTimeMillis) {
    iter++;
    const uniqueKey = Math.floor(Random.rand() * baseNum);
    assert.commandWorked(getColl(node).insert({_id: iter, x: uniqueKey}));
    assert.commandWorked(getColl(node).remove({_id: iter}));
}

jsTestLog("Kill the node");
rst.stop(node, 9, {allowedExitCode: MongoRunner.EXIT_SIGKILL});

jsTestLog("Restart the node with 'recoverFromOplogAsStandalone'");
node = rst.restart(node, {
    noReplSet: true,
    setParameter: {recoverFromOplogAsStandalone: true, logComponentVerbosity: logLevel}
});
reconnect(node);

rst.stopSet();
})();
