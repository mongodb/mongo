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

var st;
(function() {
    "use strict";

    var testDBName = "csrs_upgrade_during_migrate";
    var dataCollectionName = testDBName + ".data";
    var csrsName = jsTestName() + "-csrs";
    var numCsrsMembers;
    if (TestData.storageEngine == "wiredTiger" || TestData.storageEngine == "") {
        // TODO(schwerin): SERVER-19739 Support testing CSRS with storage engines other than wired
        // tiger, when such other storage engines support majority read concern.
        numCsrsMembers = 3;
    } else {
        numCsrsMembers = 4;
    }

    var waitUntilMaster = function (dnode) {
        var isMasterReply;
        assert.soon(function () {
            isMasterReply = dnode.adminCommand({ismaster: 1});
            return isMasterReply.ismaster;
        }, function () {
            return "Expected " + dnode.name + " to respond ismaster:true, but got " +
                tojson(isMasterReply);
        });
    };

    /*
     * If 'delayed' is true adds a 30 second slave delay to the secondary of the set referred to by
     * 'rst'.  If 'delayed' is false, removes slave delay from the secondary.
     */
    var setSlaveDelay = function(rst, delayed) {
        var conf = rst.getPrimary().getDB('local').system.replset.findOne();
        conf.version++;
        var secondaryIndex = 0;
        if (conf.members[secondaryIndex].host === rst.getPrimary().host) {
            secondaryIndex = 1;
        }
        conf.members[secondaryIndex].priority = 0;
        conf.members[secondaryIndex].hidden = true;
        conf.members[secondaryIndex].slaveDelay = delayed ? 30 : 0;
        reconfig(rst, conf);
    }

    jsTest.log("Setting up SCCC sharded cluster")
    st = new ShardingTest({
        name: "csrsUpgrade",
        mongos: 2,
        rs: { nodes: 2 },
        shards: 2,
        nopreallocj: true,
        other: {
            sync: true,
            enableBalancer: false,
            useHostname: true,
        }
    });

    var shardConfigs = st.s0.getCollection("config.shards").find().toArray();
    assert.eq(2, shardConfigs.length);
    var shard0Name = shardConfigs[0]._id;

    jsTest.log("Enabling sharding on " + testDBName + " and making " + shard0Name +
               " the primary shard");
    assert.commandWorked(st.s0.adminCommand({enablesharding: testDBName}));
    st.ensurePrimaryShard(testDBName, shard0Name);

    jsTest.log("Creating a sharded collection " + dataCollectionName);
    assert.commandWorked(st.s0.adminCommand({
        shardcollection: dataCollectionName,
        key: { _id: 1 }
    }));

    jsTest.log("Inserting data into " + dataCollectionName);
    st.s1.getCollection(dataCollectionName).insert(
        (function () {
            var result = [];
            var i;
            for (i = -20; i < 20; ++i) {
                result.push({ _id: i});
            }
            return result;
        }()), {writeConcern: {w: 'majority'}});

    jsTest.log("Introducing slave delay on shards to ensure migration is slow");
    setSlaveDelay(st.rs0, true);
    setSlaveDelay(st.rs1, true);

    jsTest.log("Restarting " + st.c0.name + " as a standalone replica set");
    var csrsConfig = {
        _id: csrsName,
        version: 1,
        configsvr: true,
        members: [ { _id: 0, host: st.c0.name }],
    };
    assert.commandWorked(st.c0.adminCommand({replSetInitiate: csrsConfig}));
    var csrs = [];
    var csrs0Opts = Object.extend({}, st.c0.fullOptions, /* deep */ true);
    csrs0Opts.restart = true;  // Don't clean the data files from the old c0.
    csrs0Opts.replSet = csrsName;
    csrs0Opts.configsvrMode = "sccc";
    MongoRunner.stopMongod(st.c0);
    csrs.push(MongoRunner.runMongod(csrs0Opts));
    waitUntilMaster(csrs[0]);

    jsTest.log("Starting new CSRS nodes");
    for (var i = 1; i < numCsrsMembers; ++i) {
        csrs.push(MongoRunner.runMongod({
            replSet: csrsName,
            configsvr: "",
            storageEngine: "wiredTiger"
        }));
        csrsConfig.members.push({ _id: i, host: csrs[i].name, votes: 0, priority: 0 });
    }
    csrsConfig.version = 2;
    jsTest.log("Adding non-voting members to csrs set: " + tojson(csrsConfig));
    assert.commandWorked(csrs[0].adminCommand({replSetReconfig: csrsConfig}));

    waitUntilAllNodesCaughtUp(csrs, 60000);

    jsTest.log("Starting long-running chunk migration");
    var joinParallelShell = startParallelShell(
        function() {
            var res = db.adminCommand({moveChunk: "csrs_upgrade_during_migrate.data",
                                       find: { _id: 0 },
                                       to: 'csrsUpgrade-rs1'
                                      });
            assert.commandFailedWithCode(res, ErrorCodes.IncompatibleCatalogManager);
        }, st.s.port);

    // Wait for migration to start
    assert.soon(function() {
                    return st.s0.getDB('config').changelog.findOne({what: 'moveChunk.start'});
                });

    // Only shut down one of the SCCC config servers to avoid any period without any config servers
    // online.
    jsTest.log("Shutting down third SCCC config server node");
    MongoRunner.stopMongod(st.c2);

    csrsConfig.members.forEach(function (member) { member.votes = 1; member.priority = 1});
    csrsConfig.version = 3;
    jsTest.log("Allowing all csrs members to vote: " + tojson(csrsConfig));
    assert.commandWorked(csrs[0].adminCommand({replSetReconfig: csrsConfig}));

    jsTest.log("Restarting " + csrs[0].name + " in csrs mode");
    delete csrs0Opts.configsvrMode;
    try {
        csrs[0].adminCommand({replSetStepDown: 60});
    } catch (e) {} // Expected
    MongoRunner.stopMongod(csrs[0]);
    csrs[0] = MongoRunner.runMongod(csrs0Opts);
    var csrsStatus;
    assert.soon(function () {
        csrsStatus = csrs[0].adminCommand({replSetGetStatus: 1});
        if (csrsStatus.members[0].stateStr == "STARTUP" ||
            csrsStatus.members[0].stateStr == "STARTUP2" ||
            csrsStatus.members[0].stateStr == "RECOVERING") {
            // Make sure first node is fully online or else mongoses still in SCCC mode might not
            // find any node online to talk to.
            return false;
        }

        var i;
        for (i = 0; i < csrsStatus.members.length; ++i) {
            if (csrsStatus.members[i].name == csrs[0].name) {
                var supportsCommitted =
                    csrs[0].getDB("admin").serverStatus().storageEngine.supportsCommittedReads;
                var stateIsRemoved = csrsStatus.members[i].stateStr == "REMOVED";
                // If the storage engine supports committed reads, it shouldn't go into REMOVED
                // state, but if it does not then it should.
                if (supportsCommitted) {
                    assert(!stateIsRemoved);
                } else if (!stateIsRemoved) {
                    return false;
                }
            }
            if (csrsStatus.members[i].stateStr == "PRIMARY") {
                return csrs[i].adminCommand({ismaster: 1}).ismaster;
            }
        }
        return false;
    }, function() {
        return "No primary or non-WT engine not removed in " + tojson(csrsStatus);
    });

    joinParallelShell(); // This will verify that the migration failed with the expected code

    jsTest.log("Shutting down final SCCC config server now that upgrade is complete");
    MongoRunner.stopMongod(st.c1);

    jsTest.log("Ensure that leftover distributed locks don't prevent future migrations");
    // Remove slave delay so that the migration can finish in a reasonable amount of time.
    st.rs0.nodes.map(m=>m.getDB("admin").setLogLevel(2,"replication")); // BF-1637
    st.rs1.nodes.map(m=>m.getDB("admin").setLogLevel(2,"replication")); // BF-1637
    setSlaveDelay(st.rs0, false);
    setSlaveDelay(st.rs1, false);
    st.rs0.awaitReplication(60000);
    st.rs1.awaitReplication(60000);

    // Due to SERVER-20290 the recipient shard may not immediately realize that the migration that
    // was going on during the upgrade has been aborted, so we need to wait until it notices this
    // before starting a new migration.
    // TODO(spencer): Remove this after SERVER-20290 is fixed.
    jsTest.log("Waiting for previous migration to be fully cleaned up");
    assert.soon(function() {
                   var res = st.rs1.getPrimary().adminCommand('_recvChunkStatus');
                   assert.commandWorked(res);
                   if (res.active) {
                       printjson(res);
                   }
                   return !res.active;
                });

    jsTest.log("Starting new migration after upgrade, which should succeed");
    assert.commandWorked(st.s0.adminCommand({moveChunk: dataCollectionName,
                                             find: { _id: 0 },
                                             to: shardConfigs[1]._id
                                            }));

}());
