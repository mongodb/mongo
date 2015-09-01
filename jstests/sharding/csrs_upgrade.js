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
 */
var st;
(function() {
    "use strict";

    var testDBName = jsTestName();
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

    var nextSplit = 0;

    /**
     * Runs a split command with a never-before used middle split point. Returns the command result.
     */
    var runNextSplit = function (snode) {
        var splitPoint = nextSplit;
        nextSplit += 10;
        return snode.adminCommand({split: dataCollectionName, middle: { _id: splitPoint }});
    };

    /**
     * Runs a config.version read, then splits the data collection and expects both operations to
     * succeed.
     */
    var assertCanSplit = function (snode, msg) {
        if (msg) {
            jsTest.log("Confirming that " + snode.name + " CAN run a split " + msg);
        }
        assert(snode.getCollection("config.version").findOne());
        assert.commandWorked(runNextSplit(snode));
    };

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

    /**
     * Runs a config.version read, then splits the data collection and expects the read to succed
     * and the split to fail.
     */
    var assertCannotSplit = function (snode, msg) {
        jsTest.log("Confirming that " + snode.name + " CANNOT run a split " + msg);
        assert(snode.getCollection("config.version").findOne());
        assert.commandFailed(runNextSplit(snode));
    };

    jsTest.log("Setting up SCCC sharded cluster")
    st = new ShardingTest({
        name: "csrsUpgrade",
        mongos: 2,
        rs: { nodes: 3 },
        shards: 2,
        nopreallocj: true,
        other: {
            sync: true,
            enableBalancer: false,
            useHostname: true
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
    assertCanSplit(st.s0);
    assert.commandWorked(st.s0.adminCommand({
        moveChunk: dataCollectionName,
        find: { _id: 0 },
        to: shardConfigs[1]._id
    }));

    jsTest.log("Inserting data into " + dataCollectionName);
    st.s1.getCollection(dataCollectionName).insert(
        (function () {
            var result = [];
            var i;
            for (i = -20; i < 20; ++i) {
                result.push({ _id: i });
            }
            return result;
        }()));

    jsTest.log("Restarting " + st.c0.name + " as a standalone replica set");
    var csrsConfig = {
        _id: csrsName,
        version: 1,
        configsvr: true,
        members: [ { _id: 0, host: st.c0.name }]
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

    assertCanSplit(st.s0, "using SCCC protocol when first config server is a 1-node replica set");

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

    jsTest.log("Splitting a chunk to confirm that the SCCC protocol works w/ 1 rs " +
               "node with secondaries");
    assertCanSplit(st.s0, "using SCCC protocol when first config server is primary of " +
                  csrs.length + "-node replica set");

    // This write is an easy way to wait for all members of the CSRS set to have
    // replicated all of the documents.
    assert.writeOK(csrs[0].getCollection('config.tmp').insert({},
                                                              { writeConcern: { w:csrs.length }}));

    jsTest.log("Shutting down second and third SCCC config server nodes");
    MongoRunner.stopMongod(st.c1);
    MongoRunner.stopMongod(st.c2);

    assertCannotSplit(st.s0, "with two SCCC nodes down");

    csrsConfig.members.forEach(function (member) { member.votes = 1; member.priority = 1});
    csrsConfig.version = 3;
    jsTest.log("Allowing all csrs members to vote: " + tojson(csrsConfig));
    assert.commandWorked(csrs[0].adminCommand({replSetReconfig: csrsConfig}));

    assertCannotSplit(st.s0, "with two SCCC nodes down, even though CSRS is almost ready");

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
        var i;
        for (i = 0; i < csrsStatus.members.length; ++i) {
            if (TestData.storageEngine != "wiredTiger" && TestData.storageEngine != "") {
                // NOTE: "" means default storage engine, which is WiredTiger.
                if (csrsStatus.members[i].name == csrs[0].name &&
                    csrsStatus.members[i].stateStr != "REMOVED") {

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

    var sconfig = Object.extend({}, st.s0.fullOptions, /* deep */ true);
    delete sconfig.port;
    sconfig.configdb = csrsName + "/" + csrs[0].name;
    assertCanSplit(MongoRunner.runMongos(sconfig),
                   "when mongos started with --configdb=" + sconfig.configdb);
    sconfig.configdb = st.s0.fullOptions.configdb;
    assertCanSplit(MongoRunner.runMongos(sconfig),
                   "when mongos started with --configdb=" + sconfig.configdb);
    assertCanSplit(st.s0, "on mongos that drove the upgrade");
    assertCanSplit(st.s1, "on mongos that was previously unaware of the upgrade");
}());
