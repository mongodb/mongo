/**
 * Tests that a standalone succeeds when passed the 'recoverFromOplogAsStandalone' parameter.
 *
 * This test only makes sense for storage engines that support recover to stable timestamp.
 * @tags: [
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   requires_replication,
 * ]
 */

(function() {
"use strict";
load("jstests/replsets/rslib.js");
load("jstests/libs/write_concern_util.js");

const name = 'standalone_replication_recovery';
const dbName = name;
const collName = 'srr_coll';
const logLevel = tojson({storage: {recovery: 2}});

const rst = new ReplSetTest({
    nodes: 2,
});

function getColl(conn) {
    return conn.getDB(dbName)[collName];
}

function assertDocsInColl(node, nums) {
    let results = getColl(node).find().sort({_id: 1}).toArray();
    let expected = nums.map((i) => ({_id: i}));
    if (!friendlyEqual(results, expected)) {
        rst.dumpOplog(node, {}, 100);
    }
    assert.eq(results, expected, "actual (left) != expected (right)");
}

jsTestLog("Test that an empty standalone fails trying to recover.");
assert.throws(
    () => rst.start(0, {noReplSet: true, setParameter: {recoverFromOplogAsStandalone: true}}));

jsTestLog("Initiating as a replica set.");
// Restart as a replica set node without the flag so we can add operations to the oplog.
let nodes = rst.startSet({setParameter: {logComponentVerbosity: logLevel}});
let node = nodes[0];
let secondary = nodes[1];
rst.initiate(
    {_id: name, members: [{_id: 0, host: node.host}, {_id: 2, host: secondary.host, priority: 0}]});

// The default WC is majority and stopServerReplication will prevent satisfying any majority writes.
assert.commandWorked(rst.getPrimary().adminCommand(
    {setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}, writeConcern: {w: "majority"}}));

// Create the collection with w:majority and then perform a clean restart to ensure that
// the collection is in a stable checkpoint.
assert.commandWorked(node.getDB(dbName).runCommand(
    {create: collName, writeConcern: {w: "majority", wtimeout: ReplSetTest.kDefaultTimeoutMS}}));
assertDocsInColl(node, []);
node = rst.restart(node, {"noReplSet": false});
reconnect(node);
assert.eq(rst.getPrimary(), node);

// Keep node 0 the primary, but prevent it from committing any writes.
stopServerReplication(secondary);

assert.commandWorked(getColl(node).insert({_id: 3}, {writeConcern: {w: 1, j: 1}}));
assert.commandWorked(getColl(node).insert({_id: 4}, {writeConcern: {w: 1, j: 1}}));
assert.commandWorked(getColl(node).insert({_id: 5}, {writeConcern: {w: 1, j: 1}}));
assertDocsInColl(node, [3, 4, 5]);

jsTestLog("Test that if we kill the node, recovery still plays.");
rst.stop(node, 9, {allowedExitCode: MongoRunner.EXIT_SIGKILL});
node = rst.restart(node, {"noReplSet": false});
reconnect(node);
assert.eq(rst.getPrimary(), node);
assertDocsInColl(node, [3, 4, 5]);

jsTestLog("Test that a replica set node cannot start up with the parameter set.");
assert.throws(
    () => rst.restart(
        0, {setParameter: {recoverFromOplogAsStandalone: true, logComponentVerbosity: logLevel}}));

jsTestLog("Test that on restart as a standalone we only see committed writes by default.");
node = rst.start(node, {noReplSet: true, setParameter: {logComponentVerbosity: logLevel}}, true);
reconnect(node);
assertDocsInColl(node, []);

// Test that we can run the validate command on a standalone.
assert.commandWorked(node.getDB(dbName).runCommand({"validate": collName}));

jsTestLog("Test that on restart with the flag set we play recovery.");
node = rst.restart(node, {
    noReplSet: true,
    setParameter: {recoverFromOplogAsStandalone: true, logComponentVerbosity: logLevel}
});
reconnect(node);
assertDocsInColl(node, [3, 4, 5]);

// Test that we can run the validate command on a standalone that recovered.
assert.commandWorked(node.getDB(dbName).runCommand({"validate": collName}));

jsTestLog("Test that we go into read-only mode.");
assert.commandFailedWithCode(getColl(node).insert({_id: 1}), ErrorCodes.IllegalOperation);

jsTestLog("Test that we cannot set the parameter during standalone runtime.");
assert.commandFailed(node.adminCommand({setParameter: 1, recoverFromOplogAsStandalone: true}));
assert.commandFailed(node.adminCommand({setParameter: 1, recoverFromOplogAsStandalone: false}));

jsTestLog("Test that on restart after standalone recovery we do not see replicated writes.");
node = rst.restart(node, {
    noReplSet: true,
    setParameter: {recoverFromOplogAsStandalone: false, logComponentVerbosity: logLevel}
});
reconnect(node);
assertDocsInColl(node, []);
assert.commandWorked(getColl(node).insert({_id: 6}));
assertDocsInColl(node, [6]);
node = rst.restart(node, {
    noReplSet: true,
    setParameter: {recoverFromOplogAsStandalone: true, logComponentVerbosity: logLevel}
});
reconnect(node);
assertDocsInColl(node, [3, 4, 5, 6]);

jsTestLog("Test that we can restart again as a replica set node.");
node = rst.restart(node, {
    noReplSet: false,
    setParameter: {recoverFromOplogAsStandalone: false, logComponentVerbosity: logLevel}
});
reconnect(node);
assert.eq(rst.getPrimary(), node);
assertDocsInColl(node, [3, 4, 5, 6]);

jsTestLog("Test that we cannot set the parameter during replica set runtime.");
assert.commandFailed(node.adminCommand({setParameter: 1, recoverFromOplogAsStandalone: true}));
assert.commandFailed(node.adminCommand({setParameter: 1, recoverFromOplogAsStandalone: false}));

jsTestLog("Test that we can still recover as a standalone.");
assert.commandWorked(getColl(node).insert({_id: 7}));
assertDocsInColl(node, [3, 4, 5, 6, 7]);
node = rst.restart(node, {
    noReplSet: true,
    setParameter: {recoverFromOplogAsStandalone: false, logComponentVerbosity: logLevel}
});
reconnect(node);
assertDocsInColl(node, [6]);
node = rst.restart(node, {
    noReplSet: true,
    setParameter: {recoverFromOplogAsStandalone: true, logComponentVerbosity: logLevel}
});
reconnect(node);
assertDocsInColl(node, [3, 4, 5, 6, 7]);

jsTestLog("Restart as a replica set node so that the test can complete successfully.");
node = rst.restart(node, {
    noReplSet: false,
    setParameter: {recoverFromOplogAsStandalone: false, logComponentVerbosity: logLevel}
});
reconnect(node);
assert.eq(rst.getPrimary(), node);
assertDocsInColl(node, [3, 4, 5, 6, 7]);

restartServerReplication(secondary);

// Skip checking db hashes since we do a write as a standalone.
TestData.skipCheckDBHashes = true;
rst.stopSet();
})();
