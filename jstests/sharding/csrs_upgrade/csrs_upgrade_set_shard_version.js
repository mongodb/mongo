/**
 * This test performs an upgrade from SCCC config servers to CSRS config servers.
 *
 * After upgrading the config servers it performs a simple query and then verifies that the
 * setShardVersion call that the query sent was sufficient to trigger the shard mongods
 * to swap their in-memory catalog manager to CSRS mode.
 *
 * This test restarts nodes and expects the data to still be present.
 * @tags: [requires_persistence]
 */

load("jstests/libs/csrs_upgrade_util.js");

(function() {
    "use strict";

    var isCSRSConnectionString = function(connStr) {
        var setAndHosts = connStr.split('/');
        return 2 == setAndHosts.length;
    };

    var assertCatalogSwapHappens = function(shardConn) {
        var connstr;
        assert.soon(
            function() {
                connstr = shardConn.adminCommand('serverStatus').sharding.configsvrConnectionString;
                return isCSRSConnectionString(connstr);
            },
            function() {
                return "Shard " + shardConn.host +
                    " still has an SCCC connection string for the config servers: " + connstr;
            });
    };

    var coordinator = new CSRSUpgradeCoordinator();
    coordinator.setupSCCCCluster();

    assert.commandWorked(coordinator.getMongos(0).adminCommand(
        {split: coordinator.getShardedCollectionName(), middle: {_id: 0}}));

    assert.commandWorked(coordinator.getMongos(0).adminCommand({
        moveChunk: coordinator.getShardedCollectionName(),
        find: {_id: 0},
        to: coordinator.getShardName(1)
    }));

    jsTest.log("Inserting data into " + coordinator.getShardedCollectionName());
    coordinator.getMongos(1)
        .getCollection(coordinator.getShardedCollectionName())
        .insert((function() {
            var result = [];
            var i;
            for (i = -20; i < 20; ++i) {
                result.push({_id: i});
            }
            return result;
        }()));

    coordinator.restartFirstConfigAsReplSet();
    coordinator.startNewCSRSNodes();
    coordinator.waitUntilConfigsCaughtUp();
    coordinator.shutdownOneSCCCNode();
    coordinator.allowAllCSRSNodesToVote();
    coordinator.switchToCSRSMode();
    assert.commandWorked(coordinator.getMongos(0).adminCommand('flushRouterConfig'));

    // Can't assert that the shard *hasn't* switched to CSRS mode yet as the dist lock pinger could
    // trigger the swap even with no other operations happening.  Instead just verify that after a
    // query the shards have definitely switched to CSRS mode.
    assert.eq(40,
              coordinator.getMongos(0)
                  .getCollection(coordinator.getShardedCollectionName())
                  .find()
                  .itcount());

    assertCatalogSwapHappens(coordinator.getShard(0));
    assertCatalogSwapHappens(coordinator.getShard(1));

}());
