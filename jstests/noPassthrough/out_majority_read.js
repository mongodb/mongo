/**
 * Tests that $out with readConcern majority will only see committed data.
 *
 * Each operation is tested on a single node, and (if supported) through merizos on both sharded and
 * unsharded collections. Merizos doesn't directly handle readConcern majority, but these tests
 * should ensure that it correctly propagates the setting to the shards when running commands.
 * @tags: [requires_sharding, requires_majority_read_concern]
 */

(function() {
    'use strict';

    // Skip this test if running with --nojournal and WiredTiger.
    if (jsTest.options().noJournal &&
        (!jsTest.options().storageEngine || jsTest.options().storageEngine === "wiredTiger")) {
        print("Skipping test because running WiredTiger without journaling isn't a valid" +
              " replica set configuration");
        return;
    }

    const testServer = MerizoRunner.runMerizod();
    const db = testServer.getDB("test");
    if (!db.serverStatus().storageEngine.supportsCommittedReads) {
        print("Skipping read_majority.js since storageEngine doesn't support it.");
        MerizoRunner.stopMerizod(testServer);
        return;
    }
    MerizoRunner.stopMerizod(testServer);

    function runTests(sourceColl, merizodConnection) {
        function makeSnapshot() {
            return assert.commandWorked(merizodConnection.adminCommand("makeSnapshot")).name;
        }
        function setCommittedSnapshot(snapshot) {
            assert.commandWorked(merizodConnection.adminCommand({"setCommittedSnapshot": snapshot}));
        }
        const db = sourceColl.getDB();
        const targetColl = db.targetColl;
        const targetReplaceDocsColl = db.targetReplaceDocsColl;

        assert.commandWorked(sourceColl.remove({}));
        assert.commandWorked(targetColl.remove({}));
        assert.commandWorked(targetReplaceDocsColl.remove({}));
        setCommittedSnapshot(makeSnapshot());

        // Insert a single document and make it visible by advancing the snapshot.
        assert.commandWorked(sourceColl.insert({_id: 1, state: 'before'}));
        assert.commandWorked(targetReplaceDocsColl.insert({_id: 1, state: 'before'}));
        setCommittedSnapshot(makeSnapshot());

        // This insert will not be visible to $out.
        assert.commandWorked(sourceColl.insert({_id: 2, state: 'before'}));
        assert.commandWorked(targetReplaceDocsColl.insert({_id: 2, state: 'before'}));
        // Similarly this update will not be visible.
        assert.commandWorked(sourceColl.update({_id: 1}, {state: 'after'}));
        assert.commandWorked(targetReplaceDocsColl.update({_id: 1}, {state: 'after'}));

        // Make sure we see only the first document.
        let res = sourceColl.aggregate([], {readConcern: {level: 'majority'}});
        assert.eq(res.itcount(), 1);

        // Run $out with the insertDocuments mode. It will pick only the first document. Also it
        // will not see the update ('after').
        res = sourceColl.aggregate(
            [
              {$match: {state: 'before'}},
              {$project: {state: 'out'}},
              {
                $out: {
                    to: targetColl.getName(),
                    db: targetColl.getDB().getName(),
                    mode: "insertDocuments"
                }
              }
            ],
            {readConcern: {level: 'majority'}});

        assert.eq(res.itcount(), 0);

        // Validate the insertDocuments results.
        res = targetColl.find().sort({_id: 1});
        // Only a single document is visible ($out did not see the second insert).
        assert.docEq(res.next(), {_id: 1, state: 'out'});
        assert(res.isExhausted());

        // The same $out but in replaceDocuments.
        res = sourceColl.aggregate(
            [
              {$match: {state: 'before'}},
              {$project: {state: 'out'}},
              {
                $out: {
                    to: targetReplaceDocsColl.getName(),
                    db: targetReplaceDocsColl.getDB().getName(),
                    mode: "replaceDocuments"
                }
              }
            ],
            {readConcern: {level: 'majority'}});
        assert.eq(res.itcount(), 0);

        setCommittedSnapshot(makeSnapshot());

        // Validate the replaceDocuments results.
        res = targetReplaceDocsColl.find().sort({_id: 1});
        // The first document must overwrite the update that the read portion of $out did not see.
        assert.docEq(res.next(), {_id: 1, state: 'out'});
        // The second document is the result of the independent insert that $out did not see.
        assert.docEq(res.next(), {_id: 2, state: 'before'});
        assert(res.isExhausted());

        assert.commandWorked(targetColl.remove({}));
        setCommittedSnapshot(makeSnapshot());

        // Insert a document that will collide with $out insert. The insert is not majority
        // commited.
        assert.commandWorked(targetColl.insert({_id: 1, state: 'collision'}));

        res = db.runCommand({
            aggregate: sourceColl.getName(),
            pipeline: [
                {$project: {state: 'out'}},
                {
                  $out: {
                      to: targetColl.getName(),
                      db: targetColl.getDB().getName(),
                      mode: "insertDocuments"
                  }
                }
            ],
            cursor: {},
            readConcern: {level: 'majority'}
        });

        assert.commandFailedWithCode(res, ErrorCodes.DuplicateKey);

        // Remove the documents (not majority).
        assert.commandWorked(targetColl.remove({_id: 1}));
        assert.commandWorked(targetColl.remove({_id: 2}));

        // $out should successfuly 'overwrite' the collection as it is 'empty' (not majority).
        res = targetReplaceDocsColl.aggregate(
            [
              {$match: {state: 'before'}},
              {$project: {state: 'out'}},
              {
                $out: {
                    to: targetColl.getName(),
                    db: targetColl.getDB().getName(),
                    mode: "insertDocuments"
                }
              }
            ],
            {readConcern: {level: 'majority'}});

        assert.eq(res.itcount(), 0);

        setCommittedSnapshot(makeSnapshot());

        // Validate the insertDocuments results.
        res = targetColl.find().sort({_id: 1});
        // Only a single document is visible ($out did not see the second insert).
        assert.docEq(res.next(), {_id: 2, state: 'out'});
        assert(res.isExhausted());
    }

    const replTest = new ReplSetTest({
        nodes: 1,
        oplogSize: 2,
        nodeOptions: {
            setParameter: 'testingSnapshotBehaviorInIsolation=true',
            enableMajorityReadConcern: '',
            shardsvr: ''
        }
    });
    replTest.startSet();
    // Cannot wait for a stable recovery timestamp with 'testingSnapshotBehaviorInIsolation' set.
    replTest.initiateWithAnyNodeAsPrimary(
        null, "replSetInitiate", {doNotWaitForStableRecoveryTimestamp: true});

    const merizod = replTest.getPrimary();

    (function testSingleNode() {
        const db = merizod.getDB("singleNode");
        runTests(db.collection, merizod);
    })();

    const shardingTest = new ShardingTest({
        shards: 0,
        merizos: 1,
    });
    assert(shardingTest.adminCommand({addShard: replTest.getURL()}));

    (function testUnshardedDBThroughMerizos() {
        const db = shardingTest.getDB("throughMerizos");
        runTests(db.unshardedDB, merizod);
    })();

    shardingTest.adminCommand({enableSharding: 'throughMerizos'});

    (function testUnshardedCollectionThroughMerizos() {
        const db = shardingTest.getDB("throughMerizos");
        runTests(db.unshardedCollection, merizod);
    })();

    (function testShardedCollectionThroughMerizos() {
        const db = shardingTest.getDB("throughMerizos");
        const collection = db.shardedCollection;
        shardingTest.adminCommand({shardCollection: collection.getFullName(), key: {_id: 1}});
        runTests(collection, merizod);
    })();

    shardingTest.stop();
    replTest.stopSet();
})();
