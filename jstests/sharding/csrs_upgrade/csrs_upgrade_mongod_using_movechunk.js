/*
 * This test uses moveChunk to initialize the sharding state on a mongod,
 * but uses an SCCC connection string for the config servers even though
 * the config servers are actually in CSRS mode, to force the mongod to
 * perform the in-memory upgrade process from SCCC mode to CSRS mode.
 *
 * This test restarts nodes and expects the collection metadata to still be present.
 * @tags: [requires_persistence]
 */

var st;
(function() {
    "use strict";

    var testDBName = jsTestName();
    var dataCollectionName = testDBName + ".data";

    jsTest.log("Setting up CSRS sharded cluster");
    st = new ShardingTest({name: "csrs", mongos: 2, shards: 2});

    jsTest.log("Enabling sharding on " + testDBName);
    assert.commandWorked(st.s0.adminCommand({enablesharding: testDBName}));

    // Ensure shard0 as the primary shard so that after it is restarted, it can still perform a
    // moveChunk (if it had no chunks, the moveChunk would fail).
    jsTest.log("Ensuring " + st.shard0.name + " as primary shard");
    st.ensurePrimaryShard(testDBName, st.shard0.name);

    jsTest.log("Creating a sharded collection " + dataCollectionName);
    assert.commandWorked(st.s0.adminCommand({shardcollection: dataCollectionName, key: {_id: 1}}));

    jsTest.log("Inserting data into " + dataCollectionName);
    st.s1.getCollection(dataCollectionName)
        .insert((function() {
            var result = [];
            var i;
            for (i = -20; i < 20; ++i) {
                result.push({_id: i});
            }
            return result;
        }()));

    jsTest.log("Splitting sharded collection " + dataCollectionName);
    var splitCmd = {
        split: dataCollectionName,
        middle: {_id: 0}
    };
    assert.commandWorked(st.s0.adminCommand(splitCmd));

    jsTest.log("Stopping and restarting mongod " + st.shard0.name +
               " to wipe its sharding awareness");
    st.shard0.restart = true;
    MongoRunner.stopMongod(st.shard0);
    var restartedMongod = MongoRunner.runMongod(st.shard0);
    assert.isnull(restartedMongod.adminCommand({serverStatus: 1}).sharding);

    jsTest.log("Sending moveChunk command with SCCC configDB string " + "to restarted mongod");
    // We construct a moveChunk command that mimics what a mongos
    // sends to a mongod in order to purposely send an SCCC config string.
    var maxChunkSizeBytes = 52428800;
    var configSCCCString = st.generateSCCCFromCSRSConnectionString(st._configDB);
    var res = st.s.adminCommand({getShardVersion: dataCollectionName});
    assert.commandWorked(res);
    var shardVersion = res.version;
    var epoch = res.versionEpoch;
    var moveChunkCmd = {
        moveChunk: dataCollectionName,
        from: st.shard0.name,
        to: st.shard1.name,
        fromShard: st.shard0.shardName,
        toShard: st.shard1.shardName,
        min: {_id: 0.0},
        max: {_id: MaxKey},
        maxChunkSizeBytes: maxChunkSizeBytes,
        configdb: configSCCCString,
        secondaryThrottle: true,
        waitForDelete: false,
        maxTimeMS: 0,
        shardVersion: [shardVersion, epoch],
        epoch: epoch
    };
    assert.commandWorked(restartedMongod.adminCommand(moveChunkCmd));

    jsTest.log("Ensuring restarted mongod is now sharding-aware " + "and has CSRS config string");

    var res = st.shard1.adminCommand({serverStatus: 1});
    assert(res.sharding);
    var expectedConfigString = res.sharding.configsvrConnectionString;
    assert(expectedConfigString);
    assert(st.isCSRSConnectionString(expectedConfigString));

    var res = restartedMongod.adminCommand({serverStatus: 1});
    assert(res.sharding);
    var observedConfigString = res.sharding.configsvrConnectionString;
    assert(observedConfigString);

    assert.eq(expectedConfigString,
              observedConfigString,
              "Restarted shard mongod's config string " + observedConfigString +
                  " differs from expected config string " + expectedConfigString);

    st.stop();

}());
