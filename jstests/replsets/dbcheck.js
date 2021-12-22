/**
 * Test the dbCheck command.
 *
 * @tags: [
 *   # We need persistence as we temporarily restart nodes as standalones.
 *   requires_persistence,
 *   requires_fcv_52,
 *   assumes_against_mongod_not_mongos,
 * ]
 */

(function() {
"use strict";

// This test injects inconsistencies between replica set members; do not fail because of expected
// dbHash differences.
TestData.skipCheckDBHashes = true;

let replSet = new ReplSetTest({name: "dbCheckSet", nodes: 2});

replSet.startSet();
replSet.initiate();
replSet.awaitSecondaryNodes();

function forEachSecondary(f) {
    for (let secondary of replSet.getSecondaries()) {
        f(secondary);
    }
}

function forEachNode(f) {
    f(replSet.getPrimary());
    forEachSecondary(f);
}

let dbName = "dbCheck-test";
let collName = "dbcheck-collection";

// Clear local.system.healthlog.
function clearLog() {
    forEachNode(conn => conn.getDB("local").system.healthlog.drop());
}

function addEnoughForMultipleBatches(collection) {
    collection.insertMany([...Array(10000).keys()].map(x => ({_id: x})));
}

// Name for a collection which takes multiple batches to check and which shouldn't be modified
// by any of the tests.
let multiBatchSimpleCollName = "dbcheck-simple-collection";
addEnoughForMultipleBatches(replSet.getPrimary().getDB(dbName)[multiBatchSimpleCollName]);

function dbCheckCompleted(db) {
    return db.currentOp().inprog.filter(x => x["desc"] == "dbCheck")[0] === undefined;
}

// Wait for DeferredWriter writes to local.system.healthlog to eventually complete.
// Requires clearLog() before the test case is run.
// TODO SERVER-61765 remove this function altoghether when healthlogging becomes
// synchronous.
function dbCheckHealthLogCompleted(db, coll, maxKey, maxSize, maxCount) {
    let query = {"namespace": coll.getFullName(), "operation": "dbCheckBatch"};
    if (maxSize === undefined && maxCount === undefined && maxKey === undefined) {
        query['data.maxKey'] = {"$type": "maxKey"};
    }
    if (maxCount !== undefined) {
        query['data.count'] = maxCount;
    } else {
        if (maxSize !== undefined) {
            query['data.bytes'] = maxSize;
        } else {
            if (maxKey !== undefined) {
                query['data.maxKey'] = maxKey;
            }
        }
    }
    return db.getSiblingDB("local").system.healthlog.find(query).itcount() === 1;
}

// Wait for dbCheck to complete (on both primaries and secondaries).  Fails an assertion if
// dbCheck takes longer than maxMs.
function awaitDbCheckCompletion(db, collName, maxKey, maxSize, maxCount) {
    let start = Date.now();

    assert.soon(() => dbCheckCompleted(db), "dbCheck timed out");
    replSet.awaitSecondaryNodes();
    replSet.awaitReplication();

    forEachNode(function(node) {
        const nodeDB = node.getDB(db);
        const nodeColl = node.getDB(db)[collName];
        assert.soon(() => dbCheckHealthLogCompleted(nodeDB, nodeColl, maxKey, maxSize, maxCount),
                    "dbCheck wait for health log timed out");
    });
}

// Check that everything in the health log shows a successful and complete check with no found
// inconsistencies.
function checkLogAllConsistent(conn) {
    let healthlog = conn.getDB("local").system.healthlog;

    assert(healthlog.find().count(), "dbCheck put no batches in health log");

    let maxResult = healthlog.aggregate(
        [{$match: {operation: "dbCheckBatch"}}, {$group: {_id: 1, key: {$max: "$data.maxKey"}}}]);

    assert(maxResult.hasNext(), "dbCheck put no batches in health log");
    assert.eq(maxResult.next().key, {"$maxKey": 1}, "dbCheck batches should end at MaxKey");

    let minResult = healthlog.aggregate(
        [{$match: {operation: "dbCheckBatch"}}, {$group: {_id: 1, key: {$min: "$data.minKey"}}}]);

    assert(minResult.hasNext(), "dbCheck put no batches in health log");
    assert.eq(minResult.next().key, {"$minKey": 1}, "dbCheck batches should start at MinKey");

    // Assert no errors (i.e., found inconsistencies).
    let errs = healthlog.find({"severity": {"$ne": "info"}});
    if (errs.hasNext()) {
        assert(false, "dbCheck found inconsistency: " + tojson(errs.next()));
    }

    // Assert no failures (i.e., checks that failed to complete).
    let failedChecks = healthlog.find({"operation": "dbCheckBatch", "data.success": false});
    if (failedChecks.hasNext()) {
        assert(false, "dbCheck batch failed: " + tojson(failedChecks.next()));
    }

    // Finds an entry with data.minKey === MinKey, and then matches its maxKey against
    // another document's minKey, and so on, and then checks that the result of that search
    // has data.maxKey === MaxKey.
    let completeCoverage = healthlog.aggregate([
            {$match: {"operation": "dbCheckBatch", "data.minKey": MinKey}},
            {
              $graphLookup: {
                  from: "system.healthlog",
                  startWith: "$data.minKey",
                  connectToField: "data.minKey",
                  connectFromField: "data.maxKey",
                  as: "batchLimits",
                  restrictSearchWithMatch: {"operation": "dbCheckBatch"}
              }
            },
            {$match: {"batchLimits.data.maxKey": MaxKey}}
        ]);

    assert(completeCoverage.hasNext(), "dbCheck batches do not cover full key range");
}

// Check that the total of all batches in the health log on `conn` is equal to the total number
// of documents and bytes in `coll`.

// Returns a document with fields "totalDocs" and "totalBytes", representing the total size of
// the batches in the health log.
function healthLogCounts(healthlog) {
    let result = healthlog.aggregate([
        {$match: {"operation": "dbCheckBatch"}},
        {
            $group: {
                "_id": null,
                "totalDocs": {$sum: "$data.count"},
                "totalBytes": {$sum: "$data.bytes"}
            }
        }
    ]);

    assert(result.hasNext(), "dbCheck put no batches in health log");

    return result.next();
}

function checkTotalCounts(conn, coll) {
    let result = healthLogCounts(conn.getDB("local").system.healthlog);

    assert.eq(result.totalDocs, coll.count(), "dbCheck batches do not count all documents");

    // Calculate the size on the client side, because collection.dataSize is not necessarily the
    // sum of the document sizes.
    let size = coll.find().toArray().reduce((x, y) => x + bsonsize(y), 0);

    assert.eq(result.totalBytes, size, "dbCheck batches do not count all bytes");
}

// First check behavior when everything is consistent.
function simpleTestConsistent() {
    let primary = replSet.getPrimary();
    clearLog();

    assert.neq(primary, undefined);
    let db = primary.getDB(dbName);
    assert.commandWorked(db.runCommand({"dbCheck": multiBatchSimpleCollName}));

    awaitDbCheckCompletion(db, multiBatchSimpleCollName);

    checkLogAllConsistent(primary);
    checkTotalCounts(primary, db[multiBatchSimpleCollName]);

    forEachSecondary(function(secondary) {
        checkLogAllConsistent(secondary);
        checkTotalCounts(secondary, secondary.getDB(dbName)[multiBatchSimpleCollName]);
    });
}

// Same thing, but now with concurrent updates.
function concurrentTestConsistent() {
    let primary = replSet.getPrimary();

    let db = primary.getDB(dbName);

    // Add enough documents that dbCheck will take a few seconds.
    db[collName].insertMany([...Array(10000).keys()].map(x => ({i: x})));

    assert.commandWorked(db.runCommand({"dbCheck": collName}));

    let coll = db[collName];

    while (db.currentOp().inprog.filter(x => x["desc"] === "dbCheck").length) {
        coll.updateOne({}, {"$inc": {"i": 10}});
        coll.insertOne({"i": 42});
        coll.deleteOne({});
    }

    awaitDbCheckCompletion(db, collName);

    checkLogAllConsistent(primary);
    // Omit check for total counts, which might have changed with concurrent updates.

    forEachSecondary(secondary => checkLogAllConsistent(secondary, true));
}

simpleTestConsistent();
concurrentTestConsistent();

// Test the various other parameters.
function testDbCheckParameters() {
    let primary = replSet.getPrimary();
    let db = primary.getDB(dbName);

    // Clean up for the test.
    clearLog();

    let docSize = bsonsize({_id: 10});

    function checkEntryBounds(start, end) {
        forEachNode(function(node) {
            let healthlog = node.getDB("local").system.healthlog;
            let keyBoundsResult = healthlog.aggregate([
                {$match: {operation: "dbCheckBatch"}},
                {
                    $group:
                        {_id: null, minKey: {$min: "$data.minKey"}, maxKey: {$max: "$data.maxKey"}}
                }
            ]);

            assert(keyBoundsResult.hasNext(), "dbCheck put no batches in health log");

            let bounds = keyBoundsResult.next();
            assert.eq(bounds.minKey, start, "dbCheck minKey field incorrect");
            assert.eq(bounds.maxKey, end, "dbCheck maxKey field incorrect");

            let counts = healthLogCounts(healthlog);
            assert.eq(counts.totalDocs, end - start);
            assert.eq(counts.totalBytes, (end - start) * docSize);
        });
    }

    // Run a dbCheck on just a subset of the documents
    let start = 1000;
    let end = 9000;

    assert.commandWorked(
        db.runCommand({dbCheck: multiBatchSimpleCollName, minKey: start, maxKey: end}));

    awaitDbCheckCompletion(db, multiBatchSimpleCollName, end);

    checkEntryBounds(start, end);

    // Now, clear the health logs again,
    clearLog();

    let maxCount = 5000;

    // and do the same with a count constraint.
    assert.commandWorked(db.runCommand(
        {dbCheck: multiBatchSimpleCollName, minKey: start, maxKey: end, maxCount: maxCount}));

    // We expect it to reach the count limit before reaching maxKey.
    awaitDbCheckCompletion(db, multiBatchSimpleCollName, undefined, undefined, maxCount);
    checkEntryBounds(start, start + maxCount);

    // Finally, do the same with a size constraint.
    clearLog();
    let maxSize = maxCount * docSize;
    assert.commandWorked(db.runCommand(
        {dbCheck: multiBatchSimpleCollName, minKey: start, maxKey: end, maxSize: maxSize}));
    awaitDbCheckCompletion(db, multiBatchSimpleCollName, end, maxSize);
    checkEntryBounds(start, start + maxCount);
}

testDbCheckParameters();

// Now, test some unusual cases where the command should fail.
function testErrorOnNonexistent() {
    let primary = replSet.getPrimary();
    let db = primary.getDB("this-probably-doesnt-exist");
    assert.commandFailed(db.runCommand({dbCheck: 1}),
                         "dbCheck spuriously succeeded on nonexistent database");
    db = primary.getDB(dbName);
    assert.commandFailed(db.runCommand({dbCheck: "this-also-probably-doesnt-exist"}),
                         "dbCheck spuriously succeeded on nonexistent collection");
}

function testErrorOnSecondary() {
    let secondary = replSet.getSecondary();
    let db = secondary.getDB(dbName);
    assert.commandFailed(db.runCommand({dbCheck: collName}));
}

function testErrorOnUnreplicated() {
    let primary = replSet.getPrimary();
    let db = primary.getDB("local");

    assert.commandFailed(db.runCommand({dbCheck: "oplog.rs"}),
                         "dbCheck spuriously succeeded on oplog");
    assert.commandFailed(primary.getDB(dbName).runCommand({dbCheck: "system.profile"}),
                         "dbCheck spuriously succeeded on system.profile");
}

testErrorOnNonexistent();
testErrorOnSecondary();
testErrorOnUnreplicated();

// Test stepdown.
function testSucceedsOnStepdown() {
    let primary = replSet.getPrimary();
    let db = primary.getDB(dbName);

    let nodeId = replSet.getNodeId(primary);
    assert.commandWorked(db.runCommand({dbCheck: multiBatchSimpleCollName}));

    // Step down the primary.
    assert.commandWorked(primary.getDB("admin").runCommand({replSetStepDown: 0, force: true}));

    // Wait for the cluster to come up.
    replSet.awaitSecondaryNodes();

    // Find the node we ran dbCheck on.
    db = replSet.getSecondaries()
             .filter(function isPreviousPrimary(node) {
                 return replSet.getNodeId(node) === nodeId;
             })[0]
             .getDB(dbName);

    // Check that it's still responding.
    try {
        assert.commandWorked(db.runCommand({ping: 1}), "ping failed after stepdown during dbCheck");
    } catch (e) {
        doassert("cannot connect after dbCheck with stepdown");
    }

    // And that our dbCheck completed.
    assert(dbCheckCompleted(db), "dbCheck failed to terminate on stepdown");
}

testSucceedsOnStepdown();

// Temporarily restart the secondary as a standalone, inject an inconsistency and
// restart it back as a secondary.
function injectInconsistencyOnSecondary(cmd) {
    const secondaryConn = replSet.getSecondary();
    const secondaryNodeId = replSet.getNodeId(secondaryConn);
    replSet.stop(secondaryNodeId, {forRestart: true /* preserve dbPath */});

    const standaloneConn = MongoRunner.runMongod({
        dbpath: secondaryConn.dbpath,
        noCleanData: true,
    });

    const standaloneDB = standaloneConn.getDB(dbName);
    assert.commandWorked(standaloneDB.runCommand(cmd));

    // Shut down the secondary and restart it as a member of the replica set.
    MongoRunner.stopMongod(standaloneConn);
    replSet.start(secondaryNodeId, {}, true /*restart*/);
    replSet.awaitNodesAgreeOnPrimaryNoAuth();
}

// Just add an extra document, and test that it catches it.
function simpleTestCatchesExtra() {
    {
        const primary = replSet.getPrimary();
        const db = primary.getDB(dbName);
        db[collName].drop();
        clearLog();

        // Create the collection on the primary.
        db.createCollection(collName, {validationLevel: "off"});
    }

    replSet.awaitReplication();
    injectInconsistencyOnSecondary({insert: collName, documents: [{}]});
    replSet.awaitReplication();

    {
        const primary = replSet.getPrimary();
        const db = primary.getDB(dbName);

        assert.commandWorked(db.runCommand({dbCheck: collName}));
        awaitDbCheckCompletion(db, collName);
    }
    const errors = replSet.getSecondary().getDB("local").system.healthlog.find(
        {operation: /dbCheck.*/, severity: "error"});

    assert.eq(errors.count(),
              1,
              "expected exactly 1 inconsistency after single inconsistent insertion, found: " +
                  JSON.stringify(errors.toArray()));
}

// Test that dbCheck catches an extra index on the secondary.
function testCollectionMetadataChanges() {
    {
        const primary = replSet.getPrimary();
        const db = primary.getDB(dbName);
        db[collName].drop();
        clearLog();

        // Create the collection on the primary.
        db.createCollection(collName, {validationLevel: "off"});
    }

    replSet.awaitReplication();
    injectInconsistencyOnSecondary(
        {createIndexes: collName, indexes: [{key: {whatever: 1}, name: "whatever"}]});
    replSet.awaitReplication();

    {
        const primary = replSet.getPrimary();
        const db = primary.getDB(dbName);
        assert.commandWorked(db.runCommand({dbCheck: collName}));
        awaitDbCheckCompletion(db, collName);
    }

    const errors = replSet.getSecondary().getDB("local").system.healthlog.find(
        {"operation": /dbCheck.*/, "severity": "error", "data.success": true});

    assert.eq(errors.count(),
              1,
              "expected exactly 1 inconsistency after single inconsistent index creation, found: " +
                  JSON.stringify(errors.toArray()));

    clearLog();
}

simpleTestCatchesExtra();
testCollectionMetadataChanges();

replSet.stopSet();
})();
