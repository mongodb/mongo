/**
 * This test performs an upgrade from SCCC config servers to CSRS config servers while running
 * group commands and verifies that the group commands continue to work properly.
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

    jsTest.log("Starting group ops in the background");

    var joinParallelShell = startParallelShell(function() {
        db = db.getSiblingDB('csrs_upgrade_during_group');
        for (var i = 0; i < 1000; i++) {
            if (i % 10 == 0) {
                print("Performing group iteration: " + i);
            }
            // Force mongos to reload chunk distribution from the config servers.
            assert.commandWorked(db.adminCommand('flushRouterConfig'));

            // Group cmd doesn't work on sharded collections, so only test on the unsharded one.
            var result = db.unsharded.group({
                key: {'num': 1},
                reduce: function(cur, result) {
                    result.total += 1;
                },
                initial: {total: 0}
            });

            assert.eq(4, result.length);
            var seen = [];
            for (var j = 0; j < 4; j++) {
                assert([0, 1, 2, 3].indexOf(result[j].num) >= 0);
                assert(seen.indexOf(result[j].num) == -1);
                seen.push(result[j].num);
                assert.eq(500, result[j].total);
            }

            if (i == 0) {
                // Insert a document to a special collection to signal to main test thread that
                // the parallel shell has started running counts.
                assert.writeOK(db.signal.insert({started: true}));
            }
        }

        // Signal to main test thread that parallel shell is finished running.
        assert.writeOK(db.signal.update({}, {finished: true}));

        jsTestLog("Finished performing group ops in parallel shell");
    }, coordinator.getMongos(0).port);

    // Wait for parallel shell to start doing group ops.
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
