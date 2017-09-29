/**
 * dbcheck.js
 *
 * Test the dbCheck command.
 */

(function() {
    "use strict";

    // TODO(SERVER-31323): Re-enable when existing dbCheck issues are fixed.
    if (true)
        return;

    let nodeCount = 3;
    let replSet = new ReplSetTest({name: "dbCheckSet", nodes: nodeCount});

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

    // Wait for dbCheck to complete (on both primaries and secondaries).  Fails an assertion if
    // dbCheck takes longer than maxMs.
    function awaitDbCheckCompletion(db) {
        let start = Date.now();

        assert.soon(() => dbCheckCompleted(db), "dbCheck timed out");
        replSet.awaitSecondaryNodes();
        replSet.awaitReplication();

        // Give the health log buffers some time to flush.
        sleep(100);
    }

    // Check that everything in the health log shows a successful and complete check with no found
    // inconsistencies.
    function checkLogAllConsistent(conn) {
        let healthlog = conn.getDB("local").system.healthlog;

        assert(healthlog.find().count(), "dbCheck put no batches in health log");

        let maxResult = healthlog.aggregate([
            {$match: {operation: "dbCheckBatch"}},
            {$group: {_id: 1, key: {$max: "$data.maxKey"}}}
        ]);

        assert(maxResult.hasNext(), "dbCheck put no batches in health log");
        assert.eq(maxResult.next().key, {"$maxKey": 1}, "dbCheck batches should end at MaxKey");

        let minResult = healthlog.aggregate([
            {$match: {operation: "dbCheckBatch"}},
            {$group: {_id: 1, key: {$min: "$data.minKey"}}}
        ]);

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
        let master = replSet.getPrimary();
        clearLog();

        assert.neq(master, undefined);
        let db = master.getDB(dbName);
        assert.commandWorked(db.runCommand({"dbCheck": multiBatchSimpleCollName}));

        awaitDbCheckCompletion(db);

        checkLogAllConsistent(master);
        checkTotalCounts(master, db[multiBatchSimpleCollName]);

        forEachSecondary(function(secondary) {
            checkLogAllConsistent(secondary);
            checkTotalCounts(secondary, secondary.getDB(dbName)[multiBatchSimpleCollName]);
        });
    }

    // Same thing, but now with concurrent updates.
    function concurrentTestConsistent() {
        let master = replSet.getPrimary();

        let db = master.getDB(dbName);

        // Add enough documents that dbCheck will take a few seconds.
        db[collName].insertMany([...Array(10000).keys()].map(x => ({i: x})));

        assert.commandWorked(db.runCommand({"dbCheck": collName}));

        let coll = db[collName];

        while (db.currentOp().inprog.filter(x => x["desc"] === "dbCheck").length) {
            coll.updateOne({}, {"$inc": {"i": 10}});
            coll.insertOne({"i": 42});
            coll.deleteOne({});
        }

        awaitDbCheckCompletion(db);

        checkLogAllConsistent(master);
        // Omit check for total counts, which might have changed with concurrent updates.

        forEachSecondary(secondary => checkLogAllConsistent(secondary, true));
    }

    simpleTestConsistent();
    concurrentTestConsistent();

    // Test the various other parameters.
    function testDbCheckParameters() {
        let master = replSet.getPrimary();
        let db = master.getDB(dbName);

        // Clean up for the test.
        clearLog();

        let docSize = bsonsize({_id: 10});

        function checkEntryBounds(start, end) {
            forEachNode(function(node) {
                let healthlog = node.getDB("local").system.healthlog;
                let keyBoundsResult = healthlog.aggregate([
                    {$match: {operation: "dbCheckBatch"}},
                    {
                      $group: {
                          _id: null,
                          minKey: {$min: "$data.minKey"},
                          maxKey: {$max: "$data.maxKey"}
                      }
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

        awaitDbCheckCompletion(db);

        checkEntryBounds(start, end);

        // Now, clear the health logs again,
        clearLog();

        let maxCount = 5000;

        // and do the same with a count constraint.
        assert.commandWorked(db.runCommand(
            {dbCheck: multiBatchSimpleCollName, minKey: start, maxKey: end, maxCount: maxCount}));

        // We expect it to reach the count limit before reaching maxKey.
        awaitDbCheckCompletion(db);
        checkEntryBounds(start, start + maxCount);

        // Finally, do the same with a size constraint.
        clearLog();
        let maxSize = maxCount * docSize;
        assert.commandWorked(db.runCommand(
            {dbCheck: multiBatchSimpleCollName, minKey: start, maxKey: end, maxSize: maxSize}));
        awaitDbCheckCompletion(db);
        checkEntryBounds(start, start + maxCount);
    }

    testDbCheckParameters();

    // Now, test some unusual cases where the command should fail.
    function testErrorOnNonexistent() {
        let master = replSet.getPrimary();
        let db = master.getDB("this-probably-doesnt-exist");
        assert.commandFailed(db.runCommand({dbCheck: 1}),
                             "dbCheck spuriously succeeded on nonexistent database");
        db = master.getDB(dbName);
        assert.commandFailed(db.runCommand({dbCheck: "this-also-probably-doesnt-exist"}),
                             "dbCheck spuriously succeeded on nonexistent collection");
    }

    function testErrorOnSecondary() {
        let secondary = replSet.getSecondary();
        let db = secondary.getDB(dbName);
        assert.commandFailed(db.runCommand({dbCheck: collName}));
    }

    function testErrorOnUnreplicated() {
        let master = replSet.getPrimary();
        let db = master.getDB("local");

        assert.commandFailed(db.runCommand({dbCheck: "oplog.rs"}),
                             "dbCheck spuriously succeeded on oplog");
        assert.commandFailed(master.getDB(dbName).runCommand({dbCheck: "system.profile"}),
                             "dbCheck spuriously succeeded on system.profile");
    }

    testErrorOnNonexistent();
    testErrorOnSecondary();
    testErrorOnUnreplicated();

    // Test stepdown.
    function testSucceedsOnStepdown() {
        let master = replSet.getPrimary();
        let db = master.getDB(dbName);

        let nodeId = replSet.getNodeId(master);
        assert.commandWorked(db.runCommand({dbCheck: multiBatchSimpleCollName}));

        // Step down the master.  This will close our connection.
        try {
            master.getDB("admin").runCommand({replSetStepDown: 0, force: true});
            // (throwing an exception in the process, which we will ignore).
        } catch (e) {
        }

        // Wait for the cluster to come up.
        replSet.awaitSecondaryNodes();

        // Find the node we ran dbCheck on.
        db = replSet.getSecondaries()
                 .filter(function isPreviousMaster(node) {
                     return replSet.getNodeId(node) === nodeId;
                 })[0]
                 .getDB(dbName);

        // Check that it's still responding.
        try {
            assert.commandWorked(db.runCommand({ping: 1}),
                                 "ping failed after stepdown during dbCheck");
        } catch (e) {
            doassert("cannot connect after dbCheck with stepdown");
        }

        // And that our dbCheck completed.
        assert(dbCheckCompleted(db), "dbCheck failed to terminate on stepdown");
    }

    testSucceedsOnStepdown();

    function testFailsOnWrongFCV() {
        let master = replSet.getPrimary();
        let db = master.getDB(dbName);

        assert.commandWorked(db.runCommand({dbCheck: multiBatchSimpleCollName}));
        assert.commandWorked(
            master.getDB("admin").adminCommand({setFeatureCompatibilityVersion: "3.4"}));

        // Check that the server is still responding.
        try {
            assert.commandWorked(db.runCommand({ping: 1}),
                                 "ping failed after FCV change during dbCheck");
        } catch (e) {
            doassert("dbCheck with FCV change crashed server");
        }

        assert.commandFailed(db.runCommand({dbCheck: multiBatchSimpleCollName}));

        assert.commandWorked(
            master.getDB("admin").adminCommand({setFeatureCompatibilityVersion: "3.6"}));
    }

    testFailsOnWrongFCV();

    function collectionUuid(db, collName) {
        return db.getCollectionInfos().filter(coll => coll.name === collName)[0].info.uuid;
    }

    function getDummyOplogEntry() {
        let master = replSet.getPrimary();
        let coll = master.getDB(dbName)[collName];

        let replSetStatus =
            assert.commandWorked(master.getDB("admin").runCommand({replSetGetStatus: 1}));
        let connStatus = replSetStatus.members.filter(m => m.self)[0];
        let lastOpTime = connStatus.optime;

        let entry = master.getDB("local").oplog.rs.find().sort({$natural: -1})[0];
        entry["ui"] = collectionUuid(master.getDB(dbName), collName);
        entry["ns"] = coll.stats().ns;
        entry["ts"] = new Timestamp();

        return entry;
    }

    // Create various inconsistencies, and check that dbCheck spots them.
    function insertOnSecondaries(doc) {
        let master = replSet.getPrimary();
        let entry = getDummyOplogEntry();
        entry["op"] = "i";
        entry["o"] = doc;

        master.getDB("local").oplog.rs.insertOne(entry);
    }

    // Run an apply-ops-ish command on a secondary.
    function runCommandOnSecondaries(doc, ns) {
        let master = replSet.getPrimary();
        let entry = getDummyOplogEntry();
        entry["op"] = "c";
        entry["o"] = doc;

        if (ns !== undefined) {
            entry["ns"] = ns;
        }

        master.getDB("local").oplog.rs.insertOne(entry);
    }

    // And on a primary.
    function runCommandOnPrimary(doc) {
        let master = replSet.getPrimary();
        let entry = getDummyOplogEntry();
        entry["op"] = "c";
        entry["o"] = doc;

        master.getDB("admin").runCommand({applyOps: [entry]});
    }

    // Just add an extra document, and test that it catches it.
    function simpleTestCatchesExtra() {
        let master = replSet.getPrimary();
        let db = master.getDB(dbName);

        clearLog();

        insertOnSecondaries({_id: 12390290});

        assert.commandWorked(db.runCommand({dbCheck: collName}));
        awaitDbCheckCompletion(db);

        let nErrors = replSet.getSecondary()
                          .getDB("local")
                          .system.healthlog.find({operation: /dbCheck.*/, severity: "error"})
                          .count();

        assert.neq(nErrors, 0, "dbCheck found no errors after insertion on secondaries");
        assert.eq(nErrors, 1, "dbCheck found too many errors after single inconsistent insertion");
    }

    // Test that dbCheck catches changing various pieces of collection metadata.
    function testCollectionMetadataChanges() {
        let master = replSet.getPrimary();
        let db = master.getDB(dbName);
        db[collName].drop();
        clearLog();

        // Create the collection on the primary.
        db.createCollection(collName, {validationLevel: "off"});

        // Add an index on the secondaries.
        runCommandOnSecondaries({createIndexes: collName, v: 2, key: {"foo": 1}, name: "foo_1"},
                                dbName + ".$cmd");

        assert.commandWorked(db.runCommand({dbCheck: collName}));
        awaitDbCheckCompletion(db);

        let nErrors =
            replSet.getSecondary()
                .getDB("local")
                .system.healthlog
                .find({"operation": /dbCheck.*/, "severity": "error", "data.success": true})
                .count();

        assert.eq(nErrors, 1, "dbCheck found wrong number of errors after inconsistent `create`");

        clearLog();
    }

    simpleTestCatchesExtra();
    testCollectionMetadataChanges();
})();
