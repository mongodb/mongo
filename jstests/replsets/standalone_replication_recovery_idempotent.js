/*
 * Tests that a 'recoverFromOplogAsStandalone' with 'takeUnstableCheckpointOnShutdown' is
 * idempotent.
 *
 * This test only makes sense for storage engines that support recover to stable timestamp.
 * @tags: [requires_persistence, requires_replication,
 * requires_majority_read_concern, uses_transactions, uses_prepare_transaction,
 * # Restarting as a standalone is not supported in multiversion tests.
 * multiversion_incompatible]
 */

(function() {
"use strict";
load("jstests/replsets/rslib.js");
load("jstests/libs/write_concern_util.js");
load("jstests/core/txns/libs/prepare_helpers.js");

const name = jsTestName();
const dbName = name;
const collName1 = 'srri_coll1';
const collName2 = 'srri_coll2';

const logLevel = tojson({storage: {recovery: 2}});

const rst = new ReplSetTest({
    nodes: 2,
});

function getColl1(conn) {
    return conn.getDB(dbName)[collName1];
}

function getColl2(conn) {
    return conn.getDB(dbName)[collName2];
}

function assertDocsInColl1(node, nums) {
    let results = getColl1(node).find().sort({_id: 1}).toArray();
    let expected = nums.map((i) => ({_id: i}));
    if (!friendlyEqual(results, expected)) {
        rst.dumpOplog(node, {}, 100);
    }
    assert.eq(results, expected, "actual (left) != expected (right)");
}

function assertPrepareConflictColl2(node, id) {
    assert.sameMembers(getColl2(node).find().toArray(), [{_id: id}]);
    assert.commandFailedWithCode(
        node.getDB(dbName).runCommand(
            {update: collName2, updates: [{q: {_id: id}, u: {$inc: {x: 1}}}], maxTimeMS: 1000}),
        ErrorCodes.MaxTimeMSExpired);
}

jsTestLog("Initiating as a replica set.");
let nodes = rst.startSet({setParameter: {logComponentVerbosity: logLevel}});
let node = nodes[0];
let secondary = nodes[1];
rst.initiate(
    {_id: name, members: [{_id: 0, host: node.host}, {_id: 1, host: secondary.host, priority: 0}]});

// The default WC is majority and stopServerReplication will prevent satisfying any majority writes.
assert.commandWorked(rst.getPrimary().adminCommand(
    {setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}, writeConcern: {w: "majority"}}));

// Create two collections with w:majority and then perform a clean restart to ensure that
// the collections are in a stable checkpoint.
assert.commandWorked(getColl1(node).insert({_id: 3}, {writeConcern: {w: "majority"}}));
assert.commandWorked(getColl2(node).insert({_id: 1}, {writeConcern: {w: "majority"}}));

node = rst.restart(node, {"noReplSet": false});
reconnect(node);
assert.eq(rst.getPrimary(), node);
assertDocsInColl1(node, [3]);
assert.sameMembers(getColl2(node).find().toArray(), [{_id: 1}]);

// Keep node 0 the primary, but prevent it from committing any writes.
stopServerReplication(secondary);
assert.commandWorked(getColl1(node).insert({_id: 4}, {writeConcern: {w: 1, j: 1}}));
assertDocsInColl1(node, [3, 4]);

let session = node.startSession();
const sessionDB = session.getDatabase(dbName);
const sessionColl2 = sessionDB.getCollection(collName2);
session.startTransaction();
const txnNumber = session.getTxnNumber_forTesting();
assert.commandWorked(sessionColl2.update({_id: 1}, {_id: 1, a: 1}));
let prepareTimestamp = PrepareHelpers.prepareTransaction(session, {w: 1, j: 1});
jsTestLog("Prepared a transaction at " + tojson(prepareTimestamp));
assertPrepareConflictColl2(node, 1);

jsTestLog("Test that on restart with just 'recoverFromOplogAsStandalone' set we play recovery.");
node = rst.restart(node, {
    noReplSet: true,
    setParameter: {recoverFromOplogAsStandalone: true, logComponentVerbosity: logLevel}
});
reconnect(node);
assertDocsInColl1(node, [3, 4]);
assertPrepareConflictColl2(node, 1);
assert.commandFailedWithCode(getColl1(node).insert({_id: 7}), ErrorCodes.IllegalOperation);

jsTestLog("Test that on restart with just 'recoverFromOplogAsStandalone' we succeed" +
          " idempotently.");
node = rst.restart(node, {
    noReplSet: true,
    setParameter: {recoverFromOplogAsStandalone: true, logComponentVerbosity: logLevel}
});
reconnect(node);
assertDocsInColl1(node, [3, 4]);
assertPrepareConflictColl2(node, 1);
assert.commandFailedWithCode(getColl1(node).insert({_id: 7}), ErrorCodes.IllegalOperation);

jsTestLog("Test that on restart with both flags we succeed.");
node = rst.restart(node, {
    noReplSet: true,
    setParameter: {
        recoverFromOplogAsStandalone: true,
        takeUnstableCheckpointOnShutdown: true,
        logComponentVerbosity: logLevel
    }
});
reconnect(node);
assertDocsInColl1(node, [3, 4]);
assertPrepareConflictColl2(node, 1);
assert.commandFailedWithCode(getColl1(node).insert({_id: 7}), ErrorCodes.IllegalOperation);

jsTestLog("Test that on restart with both flags we succeed idempotently.");
node = rst.restart(node, {
    noReplSet: true,
    setParameter: {
        recoverFromOplogAsStandalone: true,
        takeUnstableCheckpointOnShutdown: true,
        logComponentVerbosity: logLevel
    }
});
reconnect(node);
assertDocsInColl1(node, [3, 4]);
assertPrepareConflictColl2(node, 1);
assert.commandFailedWithCode(getColl1(node).insert({_id: 7}), ErrorCodes.IllegalOperation);

jsTestLog("Restart as a replica set node so that we can commit the transaction");
node = rst.restart(node, {
    noReplSet: false,
    setParameter: {
        recoverFromOplogAsStandalone: false,
        takeUnstableCheckpointOnShutdown: false,
        logComponentVerbosity: logLevel
    }
});
reconnect(node);
assert.eq(rst.getPrimary(), node);
assertDocsInColl1(node, [3, 4]);
assertPrepareConflictColl2(node, 1);

restartServerReplication(secondary);
PrepareHelpers.awaitMajorityCommitted(rst, prepareTimestamp);

assertDocsInColl1(node, [3, 4]);
assertPrepareConflictColl2(node, 1);

assert.commandWorked(node.adminCommand({
    commitTransaction: 1,
    commitTimestamp: prepareTimestamp,
    lsid: session.getSessionId(),
    txnNumber: NumberLong(txnNumber),
    autocommit: false
}));
assert.sameMembers(getColl2(node).find().toArray(), [{_id: 1, a: 1}]);

rst.stopSet();
})();
