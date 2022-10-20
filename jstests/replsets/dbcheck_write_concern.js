/**
 * Test the behavior of per-batch writeConcern in dbCheck.
 *
 * @tags: [
 *   # We need persistence as we temporarily restart nodes.
 *   requires_persistence,
 *   assumes_against_mongod_not_mongos,
 * ]
 */
(function() {
"use strict";

const replSet = new ReplSetTest({
    name: "dbCheckWriteConcern",
    nodes: 2,
    nodeOptions: {setParameter: {dbCheckHealthLogEveryNBatches: 1}},
    settings: {
        // Prevent the primary from stepping down when we temporarily shut down the secondary.
        electionTimeoutMillis: 120000
    }
});
replSet.startSet();
replSet.initiate();

function forEachSecondary(f) {
    for (let secondary of replSet.getSecondaries()) {
        f(secondary);
    }
}

function forEachNode(f) {
    f(replSet.getPrimary());
    forEachSecondary(f);
}

// Clear local.system.healthlog.
function clearLog() {
    forEachNode(conn => conn.getDB("local").system.healthlog.drop());
}

const dbName = "dbCheck-writeConcern";
const collName = "test";
const primary = replSet.getPrimary();
const db = primary.getDB(dbName);
const coll = db[collName];
const healthlog = db.getSiblingDB('local').system.healthlog;

// Validate that w:majority behaves normally.
(function testWMajority() {
    clearLog();
    coll.drop();

    // Insert 1000 docs and run a few small batches to ensure we wait for write concern between
    // each one.
    const nDocs = 1000;
    const maxDocsPerBatch = 100;
    assert.commandWorked(coll.insertMany([...Array(nDocs).keys()].map(x => ({a: x}))));
    assert.commandWorked(db.runCommand({
        dbCheck: coll.getName(),
        maxDocsPerBatch: maxDocsPerBatch,
        batchWriteConcern: {w: 'majority'},
    }));

    // Confirm dbCheck logs the expected number of batches.
    assert.soon(function() {
        return (healthlog.find({operation: "dbCheckBatch", severity: "info"}).itcount() ==
                nDocs / maxDocsPerBatch);
    }, "dbCheck doesn't seem to complete", 60 * 1000);

    // Confirm there are no warnings or errors.
    assert.eq(healthlog.find({operation: "dbCheckBatch", severity: "warning"}).itcount(), 0);
    assert.eq(healthlog.find({operation: "dbCheckBatch", severity: "error"}).itcount(), 0);
})();

// Validate that w:2 behaves normally.
(function testW2() {
    clearLog();
    coll.drop();

    // Insert 1000 docs and run a few small batches to ensure we wait for write concern between
    // each one.
    const nDocs = 1000;
    const maxDocsPerBatch = 100;
    assert.commandWorked(coll.insertMany([...Array(nDocs).keys()].map(x => ({a: x}))));
    assert.commandWorked(db.runCommand({
        dbCheck: coll.getName(),
        maxDocsPerBatch: maxDocsPerBatch,
        batchWriteConcern: {w: 2},
    }));

    // Confirm dbCheck logs the expected number of batches.
    assert.soon(function() {
        return (healthlog.find({operation: "dbCheckBatch", severity: "info"}).itcount() ==
                nDocs / maxDocsPerBatch);
    }, "dbCheck doesn't seem to complete", 60 * 1000);

    // Confirm there are no warnings or errors.
    assert.eq(healthlog.find({operation: "dbCheckBatch", severity: "warning"}).itcount(), 0);
    assert.eq(healthlog.find({operation: "dbCheckBatch", severity: "error"}).itcount(), 0);
})();

// Validate that dbCheck completes with w:majority even when the secondary is down and a wtimeout is
// specified.
(function testWMajorityUnavailable() {
    clearLog();
    coll.drop();

    // Insert 1000 docs and run a few small batches to ensure we wait for write concern between
    // each one.
    const nDocs = 1000;
    const maxDocsPerBatch = 100;
    assert.commandWorked(coll.insertMany([...Array(nDocs).keys()].map(x => ({a: x}))));
    replSet.awaitReplication();

    // Stop the secondary and expect that the dbCheck batches still complete on the primary.
    const secondaryConn = replSet.getSecondary();
    const secondaryNodeId = replSet.getNodeId(secondaryConn);
    replSet.stop(secondaryNodeId, {forRestart: true /* preserve dbPath */});

    assert.commandWorked(db.runCommand({
        dbCheck: coll.getName(),
        maxDocsPerBatch: maxDocsPerBatch,
        batchWriteConcern: {w: 'majority', wtimeout: 10},
    }));

    // Confirm dbCheck logs the expected number of batches.
    assert.soon(function() {
        return (healthlog.find({operation: "dbCheckBatch", severity: "info"}).itcount() ==
                nDocs / maxDocsPerBatch);
    }, "dbCheck doesn't seem to complete", 60 * 1000);

    // Confirm dbCheck logs a warning for every batch.
    assert.soon(function() {
        return (healthlog.find({operation: "dbCheckBatch", severity: "warning"}).itcount() ==
                nDocs / maxDocsPerBatch);
    }, "dbCheck did not log writeConcern warnings", 60 * 1000);
    // There should be no errors.
    assert.eq(healthlog.find({operation: "dbCheckBatch", severity: "error"}).itcount(), 0);

    replSet.start(secondaryNodeId, {}, true /*restart*/);
    replSet.awaitNodesAgreeOnPrimaryNoAuth();
    replSet.awaitReplication();
})();

// Validate that an invalid 'w' setting still allows dbCheck to succeed when presented with a
// wtimeout.
(function testW3Unavailable() {
    clearLog();
    coll.drop();

    // Insert 1000 docs and run a few small batches to ensure we wait for write concern between
    // each one.
    const nDocs = 1000;
    const maxDocsPerBatch = 100;
    assert.commandWorked(coll.insertMany([...Array(nDocs).keys()].map(x => ({a: x}))));
    replSet.awaitReplication();

    // Stop the secondary and expect that the dbCheck batches still complete on the primary.
    const secondaryConn = replSet.getSecondary();
    const secondaryNodeId = replSet.getNodeId(secondaryConn);
    replSet.stop(secondaryNodeId, {forRestart: true /* preserve dbPath */});

    assert.commandWorked(db.runCommand({
        dbCheck: coll.getName(),
        maxDocsPerBatch: maxDocsPerBatch,
        batchWriteConcern: {w: 3, wtimeout: 10},
    }));

    // Confirm dbCheck logs the expected number of batches.
    assert.soon(function() {
        return (healthlog.find({operation: "dbCheckBatch", severity: "info"}).itcount() ==
                nDocs / maxDocsPerBatch);
    }, "dbCheck doesn't seem to complete", 60 * 1000);

    // Confirm dbCheck logs a warning for every batch.
    assert.soon(function() {
        return (healthlog.find({operation: "dbCheckBatch", severity: "warning"}).itcount() ==
                nDocs / maxDocsPerBatch);
    }, "dbCheck did not log writeConcern warnings", 60 * 1000);
    // There should be no errors.
    assert.eq(healthlog.find({operation: "dbCheckBatch", severity: "error"}).itcount(), 0);
})();

replSet.stopSet();
})();
