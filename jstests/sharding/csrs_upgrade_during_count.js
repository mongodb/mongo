/**
 * This test performs an upgrade from SCCC config servers to CSRS config servers while running
 * count commands and verifies that the count commands continue to work properly.
 *
 * This test restarts nodes and expects the data to still be present.
 * @tags: [requires_persistence]
 */

load("jstests/libs/csrs_upgrade_util.js");

(function() {
    "use strict";

    var coordinator = new CSRSUpgradeCoordinator();
    coordinator.setupSCCCCluster();

    jsTest.log("Inserting initial data");
    var res = coordinator.getMongos(0)
                  .adminCommand({split: coordinator.getShardedCollectionName(), middle: {_id: 0}});
    assert.commandWorked(res);
    assert.commandWorked(coordinator.getMongos(0).adminCommand({
        moveChunk: coordinator.getShardedCollectionName(),
        find: {_id: 0},
        to: coordinator.getShardName(1)
    }));

    var inserts = [];
    for (var i = -1000; i < 1000; i++) {
        inserts.push({_id: i, num: Math.abs(i) % 4});
    }
    assert.writeOK(
        coordinator.getShardedCollection().insert(inserts, {writeConcern: {w: 'majority'}}));
    assert.writeOK(
        coordinator.getUnshardedCollection().insert(inserts, {writeConcern: {w: 'majority'}}));
    assert.commandWorked(coordinator.getShardedCollection().createIndex({_id: 1, num: 1}));
    assert.commandWorked(coordinator.getUnshardedCollection().createIndex({_id: 1, num: 1}));

    coordinator.restartFirstConfigAsReplSet();
    coordinator.startNewCSRSNodes();
    coordinator.waitUntilConfigsCaughtUp();

    jsTest.log("Starting count ops in the background");

    var joinParallelShell = startParallelShell(function() {
        db = db.getSiblingDB('csrs_upgrade_during_count');
        for (var i = 0; i < 10000; i++) {
            if (i % 100 == 0) {
                print("Performing count iteration: " + i);
            }
            // Force mongos to reload chunk distribution from the config servers.
            assert.commandWorked(db.adminCommand('flushRouterConfig'));

            assert.eq(2000, db.unsharded.count());
            assert.eq(2000, db.sharded.count());
            assert.eq(250, db.unsharded.count({_id: {$gte: 0, $lt: 500}, num: {$in: [0, 3]}}));
            assert.eq(250, db.sharded.count({_id: {$gte: 0, $lt: 500}, num: {$in: [0, 3]}}));

            if (i == 0) {
                // Insert a document to a special collection to signal to main test thread that
                // the parallel shell has started running counts.
                assert.writeOK(db.signal.insert({started: true}));
            }
        }

        // Signal to main test thread that parallel shell is finished running.
        assert.writeOK(db.signal.update({}, {finished: true}));

        jsTestLog("Finished performing count ops in parallel shell");
    }, coordinator.getMongos(0).port);

    // Wait for parallel shell to start doing count ops.
    var signalColl = coordinator.getMongos(0).getDB(coordinator.getTestDBName()).signal;
    assert.soon(function() {
        return signalColl.findOne();
    });

    coordinator.shutdownOneSCCCNode();
    coordinator.allowAllCSRSNodesToVote();
    coordinator.switchToCSRSMode();

    // Make sure that parallel shell didn't finish running before upgrade was complete.
    assert.eq(null, signalColl.findOne({finished: true}));

    joinParallelShell();
}());
