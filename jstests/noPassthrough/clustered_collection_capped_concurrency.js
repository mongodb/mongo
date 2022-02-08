/**
 * Validate that capped clustered collections can be written to concurrently.
 *
 * @tags: [
 *   # hangAfterCollectionInserts failpoint not available on mongos.
 *   assumes_against_mongod_not_mongos,
 *   does_not_support_stepdowns,
 *   requires_fcv_53,
 *   requires_replication,
 * ]
 */
(function() {
"use strict";
load("jstests/libs/clustered_collections/clustered_collection_util.js");
load("jstests/libs/collection_drop_recreate.js");
load('jstests/libs/dateutil.js');
load("jstests/libs/fail_point_util.js");
load('jstests/libs/parallel_shell_helpers.js');

const replSet = new ReplSetTest({name: "clustered_capped_concurrency", nodes: 1});
replSet.startSet();
replSet.initiate();

// Validate that inserts on a capped collection are serialized, whereas inserts
// on a clustered capped collection are not serialized.
function validateCappedInsertConcurrency(db, coll, clustered, expectedConcurrent) {
    assertDropCollection(db, coll.getName());

    if (clustered) {
        assert.commandWorked(db.createCollection(coll.getName(), {
            clusteredIndex: {key: {_id: 1}, unique: true},
            capped: true,
            expireAfterSeconds: 24 * 60 * 60
        }));
    } else {
        assert.commandWorked(db.createCollection(coll.getName(), {capped: true, size: 100000000}));
    }

    const ns = coll.getFullName();
    const failPointName = "hangAfterCollectionInserts";
    const fp = assert.commandWorked(db.adminCommand(
        {configureFailPoint: failPointName, mode: "alwaysOn", data: {collectionNS: ns}}));
    const timesEntered = fp.count;

    const waitForParallelShellToShutDown = startParallelShell(
        funWithArgs(function(dbName, collName) {
            assert.commandWorked(db.getSiblingDB(dbName)[collName].insertOne({}));
        }, db.getName(), coll.getName()), replSet.getPrimary().port);

    assert.commandWorked(db.adminCommand({
        waitForFailPoint: failPointName,
        timesEntered: timesEntered + 1,
        maxTimeMS: kDefaultWaitForFailPointTimeout
    }));

    // There's one and only one in-flight insert to the namespace.
    assert.eq(1, db.currentOp({ns: ns, "op": "insert"}).inprog.length);
    if (expectedConcurrent) {
        // Assert the insert is not serialized - no Metadata resource acquisition.
        assert.eq(db.currentOp({ns: ns, "op": "insert"}).inprog[0].lockStats.Metadata, undefined);
    } else {
        // Assert the insert is serialized - it acquires the Metadata resource in strong exclusive
        // mode.
        assert.neq(
            db.currentOp({ns: ns, "op": "insert"}).inprog[0].lockStats.Metadata.acquireCount.W,
            undefined);
    }

    assert.commandWorked(db.adminCommand({configureFailPoint: failPointName, mode: "off"}));
    waitForParallelShellToShutDown();
    assertDropCollection(db, coll.getName());
}

// Validate on a standard replicated namespace.
{
    const dbName = jsTestName();
    const db = replSet.getPrimary().getDB(dbName);
    const coll = db.getCollection('c');
    validateCappedInsertConcurrency(db, coll, false /*clustered*/, false /*expectedConcurrent*/);
    validateCappedInsertConcurrency(db, coll, true /*clustered*/, true /*expectedConcurrent*/);
}

// Validate on an implicitly replicated namespaces.
{
    const db = replSet.getPrimary().getDB("config");
    const coll = db.getCollection('changes.c');
    validateCappedInsertConcurrency(db, coll, false /*clustered*/, false /*expectedConcurrent*/);
    // Validate change collections: clustered, capped, implicitly replicated 'config.changes.*'
    // namespace.
    validateCappedInsertConcurrency(db, coll, true /*clustered*/, true /*expectedConcurrent*/);
}

replSet.stopSet();
})();
