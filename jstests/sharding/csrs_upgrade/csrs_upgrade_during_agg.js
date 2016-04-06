/**
 * This test performs an upgrade from SCCC config servers to CSRS config servers while running
 * aggregation jobs and verifies that the aggregations continue to work properly.
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

    jsTest.log("Starting aggregation ops in the background");

    var joinParallelShell = startParallelShell(function() {
        db = db.getSiblingDB('csrs_upgrade_during_agg');
        for (var i = 0; i < 5000; i++) {
            if (i % 100 == 0) {
                print("Performing aggregation iteration: " + i);
            }
            // Force mongos to reload chunk distribution from the config servers.
            assert.commandWorked(db.adminCommand('flushRouterConfig'));

            // One aggregate that returns a cursor
            var cursor = db.unsharded.aggregate([{$group: {_id: "$num"}}, {$sort: {_id: 1}}]);
            for (var j = 0; j < 4; j++) {
                assert.eq(j, cursor.next()._id);
            }
            assert(!cursor.hasNext());

            // One aggregate that outputs to a collection
            db.sharded.aggregate([{$group: {_id: "$num"}}, {$sort: {_id: 1}}, {$out: "out"}]);
        }

        // Signal to main test thread that parallel shell is finished running.
        assert.writeOK(db.signal.insert({finished: true}));

        jsTestLog("Finished performing aggregation ops in parallel shell");
    }, coordinator.getMongos(0).port);

    var outputColl = coordinator.getMongos(0).getDB(coordinator.getTestDBName()).out;

    // Wait for parallel shell to start doing aggregate ops.
    assert.soon(function() {
        return outputColl.findOne();
    });

    coordinator.shutdownOneSCCCNode();
    coordinator.allowAllCSRSNodesToVote();
    coordinator.switchToCSRSMode();

    // Make sure that parallel shell didn't finish running before upgrade was complete.
    var signalColl = coordinator.getMongos(0).getDB(coordinator.getTestDBName()).signal;
    assert.eq(null, signalColl.findOne({finished: true}));

    joinParallelShell();

    var cursor = outputColl.find();
    for (var i = 0; i < 4; i++) {
        assert.eq(i, cursor.next()._id);
    }
    assert(!cursor.hasNext());
}());
