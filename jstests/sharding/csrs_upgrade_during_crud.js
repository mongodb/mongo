/**
 * This test performs an upgrade from SCCC config servers to CSRS config servers while various CRUD
 * operations are taking place, and verifies that the CRUD operations continue to work.
 *
 * This test restarts nodes and expects the data to still be present.
 * @tags: [requires_persistence]
 */

load("jstests/libs/csrs_upgrade_util.js");

(function() {
    "use strict";

    var coordinator = new CSRSUpgradeCoordinator();
    coordinator.setupSCCCCluster();

    coordinator.restartFirstConfigAsReplSet();
    coordinator.startNewCSRSNodes();
    coordinator.waitUntilConfigsCaughtUp();

    jsTest.log("Starting CRUD ops in the background");

    var joinParallelShell = startParallelShell(function() {
        for (var i = 1; i <= 5000; i++) {
            if (i % 100 == 0) {
                print("Performing backgroud CRUD iteration: " + i);
            }
            // Force mongos to reload chunk distribution from the config servers.
            assert.commandWorked(db.adminCommand('flushRouterConfig'));
            var coll1 = db.getSiblingDB('csrs_upgrade_during_crud').sharded;
            var coll2 = db.getSiblingDB('csrs_upgrade_during_crud').unsharded;

            assert.writeOK(coll1.insert({_id: i}));
            assert.writeOK(coll1.insert({_id: -i}));
            assert.writeOK(coll2.insert({_id: i}));
            assert.writeOK(coll2.insert({_id: -i}));
            assert.writeOK(coll1.remove({_id: -i}));
            assert.writeOK(coll2.remove({_id: -i}));
            assert.writeOK(coll1.update({_id: i}, {$inc: {updated: 1}}));
            assert.writeOK(coll2.update({_id: i}, {$inc: {updated: 1}}));

            var original = coll1.findAndModify({query: {_id: i}, update: {$inc: {updated: 1}}});
            assert.eq(i, original._id);
            assert.eq(1, original.updated);
            original = coll2.findAndModify({query: {_id: i}, update: {$inc: {updated: 1}}});
            assert.eq(i, original._id);
            assert.eq(1, original.updated);
        }

        jsTestLog("Finished performing CRUD ops in parallel shell");
    }, coordinator.getMongos(0).port);

    // Wait for parallel shell to start doing CRUD ops.
    assert.soon(function() {
        return coordinator.getUnshardedCollection().findOne();
    });

    coordinator.shutdownOneSCCCNode();
    coordinator.allowAllCSRSNodesToVote();
    coordinator.switchToCSRSMode();

    // Make sure CRUD ops didn't finish before the CSRS upgrade did.
    assert.gt(5000, coordinator.getUnshardedCollection().count());
    joinParallelShell();

    assert.eq(5000, coordinator.getShardedCollection().count());
    assert.eq(5000, coordinator.getUnshardedCollection().count());
    assert.eq(5000, coordinator.getShardedCollection().find({updated: 2}).itcount());
    assert.eq(5000, coordinator.getUnshardedCollection().find({updated: 2}).itcount());

}());
