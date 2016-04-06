/**
 * This test performs an upgrade from SCCC config servers to CSRS config servers while running
 * distinct commands and verifies that the distinct commands continue to work properly.
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

    coordinator.restartFirstConfigAsReplSet();
    coordinator.startNewCSRSNodes();
    coordinator.waitUntilConfigsCaughtUp();

    jsTest.log("Starting distinct ops in the background");

    var joinParallelShell = startParallelShell(function() {
        db = db.getSiblingDB('csrs_upgrade_during_distinct');
        for (var i = 0; i < 10000; i++) {
            if (i % 100 == 0) {
                print("Performing distinct iteration: " + i);
            }
            // Force mongos to reload chunk distribution from the config servers.
            assert.commandWorked(db.adminCommand('flushRouterConfig'));

            var values1 = db.sharded.distinct('num');
            var values2 = db.unsharded.distinct('num');
            assert.eq(4, values1.length);
            assert.eq(4, values2.length);
            for (var j = 0; j < 4; j++) {
                assert(values1.indexOf(j) >= 0);
                assert(values2.indexOf(j) >= 0);
            }

            if (i == 0) {
                // Insert a document to a special collection to signal to main test thread that
                // the parallel shell has started running counts.
                assert.writeOK(db.signal.insert({started: true}));
            }
        }

        // Signal to main test thread that parallel shell is finished running.
        assert.writeOK(db.signal.update({}, {finished: true}));

        jsTestLog("Finished performing distinct ops in parallel shell");
    }, coordinator.getMongos(0).port);

    // Wait for parallel shell to start doing distinct ops.
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
