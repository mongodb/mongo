/**
 * This test performs an upgrade from SCCC config servers to CSRS config servers while a chunk
 * migration is taking place.
 * It verifies that the migration detects when a catalog manager swap is required and aborts the
 * migration before reaching the critical section.
 *
 * This test restarts nodes and expects the data to still be present.
 * @tags: [requires_persistence]
 */
load("jstests/replsets/rslib.js");
load("jstests/libs/csrs_upgrade_util.js");

var st;
(function() {
    "use strict";

    /*
     * If 'delayed' is true adds a 30 second slave delay to the secondaries of the set referred to
     * by 'rst'.  If 'delayed' is false, removes slave delay from the secondaries.
     */
    var setSlaveDelay = function(rst, delayed) {
        var conf = rst.getPrimary().getDB('local').system.replset.findOne();
        conf.version++;
        for (var i = 0; i < conf.members.length; i++) {
            if (conf.members[i].host === rst.getPrimary().host) {
                continue;
            }
            conf.members[i].priority = 0;
            conf.members[i].hidden = true;
            conf.members[i].slaveDelay = delayed ? 30 : 0;
        }
        reconfig(rst, conf);
    };

    var coordinator = new CSRSUpgradeCoordinator();
    coordinator.setupSCCCCluster();

    jsTest.log("Inserting data into " + coordinator.getDataCollectionName());
    coordinator.getMongos(1).getCollection(coordinator.getDataCollectionName()).insert(
        (function () {
            var result = [];
            var i;
            for (i = -20; i < 20; ++i) {
                result.push({ _id: i});
            }
            return result;
        }()), {writeConcern: {w: 'majority'}});

    jsTest.log("Introducing slave delay on shards to ensure migration is slow");
    var shardRS0 = coordinator.getShardingTestFixture().rs0;
    var shardRS1 = coordinator.getShardingTestFixture().rs1;
    setSlaveDelay(shardRS0, true);
    setSlaveDelay(shardRS1, true);

    coordinator.restartFirstConfigAsReplSet();
    coordinator.startNewCSRSNodes();
    coordinator.waitUntilConfigsCaughtUp();

    jsTest.log("Starting long-running chunk migration");
    var joinParallelShell = startParallelShell(
        function() {
            var res = db.adminCommand({moveChunk: "csrs_upgrade_during_migrate.data",
                                       find: { _id: 0 },
                                       to: 'csrsUpgrade-rs1'
                                      });
            assert.commandFailedWithCode(res, ErrorCodes.IncompatibleCatalogManager);
        }, coordinator.getMongos(0).port);

    // Wait for migration to start
    assert.soon(function() {
                    var configDB = coordinator.getMongos(0).getDB('config');
                    return configDB.changelog.findOne({what: 'moveChunk.start'});
                });

    coordinator.shutdownOneSCCCNode();
    coordinator.allowAllCSRSNodesToVote();
    coordinator.switchToCSRSMode();

    joinParallelShell(); // This will verify that the migration failed with the expected code

    jsTest.log("Ensure that leftover distributed locks don't prevent future migrations");
    // Remove slave delay so that the migration can finish in a reasonable amount of time.

    setSlaveDelay(shardRS0, false);
    setSlaveDelay(shardRS1, false);
    shardRS0.awaitReplication(60000);
    shardRS1.awaitReplication(60000);

    // The recipient shard may not immediately realize that the migration that
    // was going on during the upgrade has been aborted, so we need to wait until it notices this
    // before starting a new migration.
    jsTest.log("Waiting for previous migration to be fully cleaned up");
    assert.soon(function() {
                   var res = shardRS1.getPrimary().adminCommand('_recvChunkStatus');
                   assert.commandWorked(res);
                   if (res.active) {
                       printjson(res);
                   }
                   return !res.active;
                });

    jsTest.log("Starting new migration after upgrade, which should succeed");
    assert.commandWorked(coordinator.getMongos(0).adminCommand(
            {moveChunk: coordinator.getDataCollectionName(),
             find: { _id: 0 },
             to: coordinator.getShardName(1)
            }));

}());
