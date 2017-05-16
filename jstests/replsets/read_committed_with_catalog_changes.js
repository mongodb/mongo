/**
 * Test read committed functionality when mixed with catalog changes. Since we don't support
 * multiple versions of the catalog, operations that modify the catalog may need to lock out
 * committed readers until the modification is in the committed snapshot.
 *
 * The following replicated operations are tested here:
 *  - creating a collection in an existing db
 *  - creating a collection in a new db
 *  - dropping a collection
 *  - dropping a db
 *  - dropping a collection and creating one with the same name
 *  - dropping a db and creating one with the same name
 *  - renaming a collection to a new, unused name
 *  - renaming a collection on top of an existing collection
 *  - creating a foreground index
 *  - creating a background index
 *  - dropping an index
 *
 * The following non-replicated operations are tested here:
 *  - repair database
 *  - reindex collection
 *  - compact collection
 */

load("jstests/libs/parallelTester.js");  // For ScopedThread.
load("jstests/replsets/rslib.js");       // For startSetIfSupportsReadMajority.

(function() {
    "use strict";

    // Each test case includes a 'prepare' method that sets up the initial state starting with a
    // database that has been dropped, a 'performOp' method that does some operation, and two
    // arrays, 'blockedCollections' and 'unblockedCollections', that list the collections that
    // should be blocked or unblocked between the time the operation is performed until it is
    // committed. If the operation is local only and isn't replicated, the test case should include
    // a 'localOnly' field set to true. Test cases are not allowed to touch any databases other than
    // the one passed in.
    const testCases = {
        createCollectionInExistingDB: {
            prepare: function(db) {
                assert.writeOK(db.other.insert({_id: 1}));
            },
            performOp: function(db) {
                assert.writeOK(db.coll.insert({_id: 1}));
            },
            blockedCollections: ['coll'],
            unblockedCollections: ['other'],
        },
        createCollectionInNewDB: {
            prepare: function(db) {},
            performOp: function(db) {
                assert.writeOK(db.coll.insert({_id: 1}));
            },
            blockedCollections: ['coll'],
            unblockedCollections: ['otherDoesNotExist'],  // Only existent collections are blocked.
        },
        dropCollection: {
            prepare: function(db) {
                assert.writeOK(db.other.insert({_id: 1}));
                assert.writeOK(db.coll.insert({_id: 1}));
            },
            performOp: function(db) {
                assert(db.coll.drop());
            },
            blockedCollections: [],
            unblockedCollections: ['coll', 'other'],
        },
        dropDB: {
            prepare: function(db) {
                assert.writeOK(db.coll.insert({_id: 1}));
            },
            performOp: function(db) {
                assert.commandWorked(db.dropDatabase());
            },
            blockedCollections: [],
            unblockedCollections: ['coll'],
        },
        dropAndRecreateCollection: {
            prepare: function(db) {
                assert.writeOK(db.other.insert({_id: 1}));
                assert.writeOK(db.coll.insert({_id: 1}));
            },
            performOp: function(db) {
                assert(db.coll.drop());
                assert.writeOK(db.coll.insert({_id: 1}));
            },
            blockedCollections: ['coll'],
            unblockedCollections: ['other'],
        },
        dropAndRecreateDB: {
            prepare: function(db) {
                assert.writeOK(db.coll.insert({_id: 1}));
            },
            performOp: function(db) {
                assert.commandWorked(db.dropDatabase());
                assert.writeOK(db.coll.insert({_id: 1}));
            },
            blockedCollections: ['coll'],
            unblockedCollections: ['otherDoesNotExist'],
        },
        renameCollectionToNewName: {
            prepare: function(db) {
                assert.writeOK(db.other.insert({_id: 1}));
                assert.writeOK(db.from.insert({_id: 1}));
            },
            performOp: function(db) {
                assert.commandWorked(db.from.renameCollection('coll'));
            },
            blockedCollections: ['coll'],
            unblockedCollections: ['other', 'from' /*doesNotExist*/],
        },
        renameCollectionToExistingName: {
            prepare: function(db) {
                assert.writeOK(db.other.insert({_id: 1}));
                assert.writeOK(db.from.insert({_id: 'from'}));
                assert.writeOK(db.coll.insert({_id: 'coll'}));
            },
            performOp: function(db) {
                assert.commandWorked(db.from.renameCollection('coll', true));
            },
            blockedCollections: ['coll'],
            unblockedCollections: ['other', 'from' /*doesNotExist*/],
        },
        createIndexForeground: {
            prepare: function(db) {
                assert.writeOK(db.other.insert({_id: 1}));
                assert.writeOK(db.coll.insert({_id: 1}));
            },
            performOp: function(db) {
                assert.commandWorked(db.coll.ensureIndex({x: 1}, {background: false}));
            },
            blockedCollections: ['coll'],
            unblockedCollections: ['other'],
        },
        createIndexBackground: {
            prepare: function(db) {
                assert.writeOK(db.other.insert({_id: 1}));
                assert.writeOK(db.coll.insert({_id: 1}));
            },
            performOp: function(db) {
                assert.commandWorked(db.coll.ensureIndex({x: 1}, {background: true}));
            },
            blockedCollections: ['coll'],
            unblockedCollections: ['other'],
        },
        dropIndex: {
            prepare: function(db) {
                assert.writeOK(db.other.insert({_id: 1}));
                assert.writeOK(db.coll.insert({_id: 1}));
                assert.commandWorked(db.coll.ensureIndex({x: 1}));
            },
            performOp: function(db) {
                assert.commandWorked(db.coll.dropIndex({x: 1}));
            },
            blockedCollections: ['coll'],
            unblockedCollections: ['other'],
        },

        // Remaining cases are local-only operations.
        repairDatabase: {
            prepare: function(db) {
                assert.writeOK(db.coll.insert({_id: 1}));
            },
            performOp: function(db) {
                assert.commandWorked(db.repairDatabase());
            },
            blockedCollections: ['coll'],
            unblockedCollections: ['otherDoesNotExist'],
            localOnly: true,
        },
        reIndex: {
            prepare: function(db) {
                assert.writeOK(db.other.insert({_id: 1}));
                assert.writeOK(db.coll.insert({_id: 1}));
                assert.commandWorked(db.coll.ensureIndex({x: 1}));
            },
            performOp: function(db) {
                assert.commandWorked(db.coll.reIndex());
            },
            blockedCollections: ['coll'],
            unblockedCollections: ['other'],
            localOnly: true,
        },
        compact: {
            // At least on WiredTiger, compact is fully inplace so it doesn't need to block readers.
            prepare: function(db) {
                assert.writeOK(db.other.insert({_id: 1}));
                assert.writeOK(db.coll.insert({_id: 1}));
                assert.commandWorked(db.coll.ensureIndex({x: 1}));
            },
            performOp: function(db) {
                var res = db.coll.runCommand('compact', {force: true});
                if (res.code != ErrorCodes.CommandNotSupported) {
                    // It is fine for a storage engine to support snapshots but not compact. Since
                    // compact doesn't block any collections we are fine with doing a no-op here.
                    // Other errors should fail the test.
                    assert.commandWorked(res);
                }

            },
            blockedCollections: [],
            unblockedCollections: ['coll', 'other'],
            localOnly: true,
        },
    };

    // Assertion helpers. These must get all state as arguments rather than through closure since
    // they may be passed in to a ScopedThread.
    function assertReadsBlock(coll) {
        var res =
            coll.runCommand('find', {"readConcern": {"level": "majority"}, "maxTimeMS": 5000});
        assert.commandFailedWithCode(res,
                                     ErrorCodes.ExceededTimeLimit,
                                     "Expected read of " + coll.getFullName() + " to block");
    }

    function assertReadsSucceed(coll, timeoutMs = 20000) {
        var res =
            coll.runCommand('find', {"readConcern": {"level": "majority"}, "maxTimeMS": timeoutMs});
        assert.commandWorked(res, 'reading from ' + coll.getFullName());
        // Exhaust the cursor to avoid leaking cursors on the server.
        new DBCommandCursor(coll.getMongo(), res).itcount();
    }

    // Set up a set and grab things for later.
    var name = "read_committed_with_catalog_changes";
    var replTest =
        new ReplSetTest({name: name, nodes: 3, nodeOptions: {enableMajorityReadConcern: ''}});

    if (!startSetIfSupportsReadMajority(replTest)) {
        jsTest.log("skipping test since storage engine doesn't support committed reads");
        return;
    }

    var nodes = replTest.nodeList();
    var config = {
        "_id": name,
        "members": [
            {"_id": 0, "host": nodes[0]},
            {"_id": 1, "host": nodes[1], priority: 0},
            {"_id": 2, "host": nodes[2], arbiterOnly: true}
        ]
    };

    replTest.initiate(config);

    // Get connections.
    var primary = replTest.getPrimary();
    var secondary = replTest.liveNodes.slaves[0];

    // This is the DB that all of the tests will use.
    var mainDB = primary.getDB('mainDB');

    // This DB won't be used by any tests so it should always be unblocked.
    var otherDB = primary.getDB('otherDB');
    var otherDBCollection = otherDB.collection;
    assert.writeOK(
        otherDBCollection.insert({}, {writeConcern: {w: "majority", wtimeout: 60 * 1000}}));
    assertReadsSucceed(otherDBCollection);

    for (var testName in testCases) {
        jsTestLog('Running test ' + testName);
        var test = testCases[testName];

        const setUpInitialState = function setUpInitialState() {
            assert.commandWorked(mainDB.dropDatabase());
            test.prepare(mainDB);
            mainDB.getLastError('majority', 60 * 1000);
            // Do some sanity checks.
            assertReadsSucceed(otherDBCollection);
            test.blockedCollections.forEach((name) => assertReadsSucceed(mainDB[name]));
            test.unblockedCollections.forEach((name) => assertReadsSucceed(mainDB[name]));
        };

        // All operations, whether replicated or not, must become visible automatically as long as
        // the secondary is keeping up.
        setUpInitialState();
        test.performOp(mainDB);
        assertReadsSucceed(otherDBCollection);
        test.blockedCollections.forEach((name) => assertReadsSucceed(mainDB[name]));
        test.unblockedCollections.forEach((name) => assertReadsSucceed(mainDB[name]));

        // Return to the initial state, then stop the secondary from applying new writes to prevent
        // them from becoming committed.
        setUpInitialState();
        assert.commandWorked(
            secondary.adminCommand({configureFailPoint: "rsSyncApplyStop", mode: "alwaysOn"}));

        // If the tested operation isn't replicated, do a write to the side collection before
        // performing the operation. This will ensure that the operation happens after an
        // uncommitted write which prevents it from immediately being marked as committed.
        if (test.localOnly) {
            assert.writeOK(otherDBCollection.insert({}));
        }

        // Perform the op and ensure that blocked collections block and unblocked ones don't.
        test.performOp(mainDB);
        assertReadsSucceed(otherDBCollection);
        test.blockedCollections.forEach((name) => assertReadsBlock(mainDB[name]));
        test.unblockedCollections.forEach((name) => assertReadsSucceed(mainDB[name]));

        // Use background threads to test that reads that start blocked can complete if the
        // operation they are waiting on becomes committed while the read is still blocked.
        // We don't do this when testing auth because ScopedThread's don't propagate auth
        // credentials.
        var threads = jsTest.options().auth ? [] : test.blockedCollections.map((name) => {
            // This function must get all inputs as arguments and can't use closure because it
            // is used in a ScopedThread.
            function bgThread(host, collection, assertReadsSucceed) {
                // Use a longer timeout since we expect to block for a little while (at least 2
                // seconds).
                assertReadsSucceed(new Mongo(host).getCollection(collection), 30 * 1000);
            }
            var thread = new ScopedThread(
                bgThread, primary.host, mainDB[name].getFullName(), assertReadsSucceed);
            thread.start();
            return thread;
        });
        sleep(1000);  // Give the reads a chance to block.

        try {
            // Try the committed read again after sleeping to ensure that it still blocks even if it
            // isn't immediately after the operation.
            test.blockedCollections.forEach((name) => assertReadsBlock(mainDB[name]));

            // Restart oplog application on the secondary and ensure the blocked collections become
            // unblocked.
            assert.commandWorked(
                secondary.adminCommand({configureFailPoint: "rsSyncApplyStop", mode: "off"}));
            mainDB.getLastError("majority", 60 * 1000);
            test.blockedCollections.forEach((name) => assertReadsSucceed(mainDB[name]));

            // Wait for the threads to complete and report any errors encountered from running them.
            threads.forEach((thread) => {
                thread.join();
                thread.join = () => {};  // Make join a no-op for the finally below.
                assert(!thread.hasFailed(), "One of the threads failed. See above for details.");
            });
        } finally {
            // Make sure we wait for all threads to finish.
            threads.forEach(thread => thread.join());
        }
    }
}());
