/**
 * This test performs an upgrade from SCCC config servers to CSRS config servers.
 *
 * Along the way, it confirms that the config servers always offer the
 * ability to read metadata, and checks that metadata is writable or
 * unwritable as appropriate at certain steps in the process.
 *
 * During the setup phase, a new sharded cluster is created with SCCC
 * config servers and a single sharded collection with documents on
 * each of two shards.
 *
 * During the upgrade phase, chunks are split to confirm the
 * availability or unavailability of metadata writes, and
 * config.version is read to confirm the availability of metadata
 * reads.
 *
 * This test restarts nodes and expects the data to still be present.
 * @tags: [requires_persistence]
 */

load("jstests/libs/csrs_upgrade_util.js");

var st;
(function() {
    "use strict";

    var coordinator = new CSRSUpgradeCoordinator();

    var nextSplit = 0;

    /**
     * Runs a split command with a never-before used middle split point. Returns the command result.
     */
    var runNextSplit = function (snode) {
        var splitPoint = nextSplit;
        nextSplit += 10;
        return snode.adminCommand({split: coordinator.getDataCollectionName(),
                                   middle: { _id: splitPoint }});
    };

    /**
     * Runs several basic operations against a given mongos, including a config.version read,
     * spliting the data collection, and doing basic crud ops against the data collection, and
     * expects all operations to succeed.
     */
    var assertOpsWork = function (snode, msg) {
        if (msg) {
            jsTest.log("Confirming that " + snode.name + " CAN run basic sharding ops " + msg);
        }
        assert(snode.getCollection("config.version").findOne());
        assert.commandWorked(runNextSplit(snode));

        // Check that basic crud ops work.
        var dataColl = snode.getCollection(coordinator.getDataCollectionName());
        assert.eq(40, dataColl.find().itcount());
        assert.writeOK(dataColl.insert({_id: 100, x: 1}));
        assert.writeOK(dataColl.update({_id: 100}, {$inc: {x: 1}}));
        assert.writeOK(dataColl.remove({x:2}));
    };

    /**
     * Runs a config.version read, then splits the data collection and expects the read to succed
     * and the split to fail.
     */
    var assertCannotSplit = function (snode, msg) {
        jsTest.log("Confirming that " + snode.name + " CANNOT run a split " + msg);
        assert(snode.getCollection("config.version").findOne());
        assert.commandFailed(runNextSplit(snode));
    };

    coordinator.setupSCCCCluster();

    assert.commandWorked(runNextSplit(coordinator.getMongos(0)));
    assert.commandWorked(coordinator.getMongos(0).adminCommand({
        moveChunk: coordinator.getDataCollectionName(),
        find: { _id: 0 },
        to: coordinator.getShardName(1)
    }));

    jsTest.log("Inserting data into " + coordinator.getDataCollectionName());
    coordinator.getMongos(1).getCollection(coordinator.getDataCollectionName()).insert(
        (function () {
            var result = [];
            var i;
            for (i = -20; i < 20; ++i) {
                result.push({ _id: i });
            }
            return result;
        }()));

    coordinator.restartFirstConfigAsReplSet();

    assertOpsWork(coordinator.getMongos(0),
                  "using SCCC protocol when first config server is a 1-node replica set");

    coordinator.startNewCSRSNodes();

    jsTest.log("Splitting a chunk to confirm that the SCCC protocol works w/ 1 rs " +
               "node with secondaries");
    assertOpsWork(coordinator.getMongos(0),
                  "using SCCC protocol when first config server is primary of " +
                  coordinator.getCSRSNodes().length + "-node replica set");

    coordinator.waitUntilConfigsCaughtUp();
    coordinator.shutdownOneSCCCNode();

    assertCannotSplit(coordinator.getMongos(0), "with one SCCC node down");

    coordinator.allowAllCSRSNodesToVote();

    assertCannotSplit(coordinator.getMongos(0),
                      "with one SCCC node down, even though CSRS is almost ready");

    coordinator.switchToCSRSMode();

    var sconfig = coordinator.getMongosConfig();
    sconfig.configdb = coordinator.getCSRSName() + "/" + coordinator.getCSRSNodes()[0].name;
    assertOpsWork(MongoRunner.runMongos(sconfig),
                  "when mongos started with --configdb=" + sconfig.configdb);
    sconfig = coordinator.getMongosConfig();
    assertOpsWork(MongoRunner.runMongos(sconfig),
                  "when mongos started with --configdb=" + sconfig.configdb);
    assertOpsWork(coordinator.getMongos(0), "on mongos that drove the upgrade");
    assertOpsWork(coordinator.getMongos(1), "on mongos that was previously unaware of the upgrade");
}());
