/**
 * This test performs an upgrade from SCCC config servers to CSRS config servers while running
 * map reduce jobs and verifies that the map-reduces continue to work properly.
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

    jsTest.log("Starting mapReduce ops in the background");

    var joinParallelShell = startParallelShell(function() {
        db = db.getSiblingDB('csrs_upgrade_during_mr');
        for (var i = 0; i < 1000; i++) {
            if (i % 10 == 0) {
                print("Performing map reduce iteration: " + i);
            }
            // Force mongos to reload chunk distribution from the config servers.
            assert.commandWorked(db.adminCommand('flushRouterConfig'));

            var map = function() {
                emit(this.num, 1);
            };

            var reduce = function(key, values) {
                return Array.sum(values);
            };

            var res = db.runCommand(
                {mapReduce: 'sharded', map: map, reduce: reduce, out: {replace: 'out1'}});
            assert.commandWorked(res);

            res = db.runCommand(
                {mapReduce: 'unsharded', map: map, reduce: reduce, out: {replace: 'out2'}});
            assert.commandWorked(res);
        }

        // Signal to main test thread that parallel shell is finished running.
        assert.writeOK(db.signal.insert({finished: true}));

        jsTestLog("Finished performing mapReduce ops in parallel shell");
    }, coordinator.getMongos(0).port);

    var outputColl1 = coordinator.getMongos(0).getDB(coordinator.getTestDBName()).out1;
    var outputColl2 = coordinator.getMongos(0).getDB(coordinator.getTestDBName()).out2;

    // Wait for parallel shell to start doing mapReduce ops.
    assert.soon(function() {
        return outputColl1.findOne();
    });

    coordinator.shutdownOneSCCCNode();
    coordinator.allowAllCSRSNodesToVote();
    coordinator.switchToCSRSMode();

    // Make sure that parallel shell didn't finish running before upgrade was complete.
    var signalColl = coordinator.getMongos(0).getDB(coordinator.getTestDBName()).signal;
    assert.eq(null, signalColl.findOne({finished: true}));

    joinParallelShell();

    printjson(outputColl1.find().toArray());
    assert.eq(4, outputColl1.count());
    assert.eq(4, outputColl2.count());
    outputColl1.find().forEach(function(x) {
        assert.eq(500, x.value);
    });
    outputColl2.find().forEach(function(x) {
        assert.eq(500, x.value);
    });
}());
