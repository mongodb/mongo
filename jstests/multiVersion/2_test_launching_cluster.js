//
// Tests launching multi-version ShardingTest clusters
//

load('./jstests/multiVersion/libs/verify_versions.js');

(function() {
    "use strict";
    // Check our latest versions
    var versionsToCheck = ["last-stable", "latest"];
    var versionsToCheckMongos = ["last-stable"];

    jsTest.log("Testing mixed versions...");

    // Set up a multi-version cluster
    var st = new ShardingTest({
        shards: 2,
        mongos: 2,
        other: {
            mongosOptions: {binVersion: versionsToCheckMongos},
            configOptions: {binVersion: versionsToCheck},
            shardOptions: {binVersion: versionsToCheck},
            // TODO: SERVER-24163 remove after v3.4
            waitForCSRSSecondaries: false
        }
    });

    var shards = [st.shard0, st.shard1];
    var mongoses = [st.s0, st.s1];
    var configs = [st.config0, st.config1, st.config2];

    // Make sure we have hosts of all the different versions
    var versionsFound = [];
    for (var j = 0; j < shards.length; j++)
        versionsFound.push(shards[j].getBinVersion());

    assert.allBinVersions(versionsToCheck, versionsFound);

    versionsFound = [];
    for (var j = 0; j < mongoses.length; j++)
        versionsFound.push(mongoses[j].getBinVersion());

    assert.allBinVersions(versionsToCheckMongos, versionsFound);

    versionsFound = [];
    for (var j = 0; j < configs.length; j++)
        versionsFound.push(configs[j].getBinVersion());

    assert.allBinVersions(versionsToCheck, versionsFound);

    st.stop();

    jsTest.log("Testing mixed versions with replica sets...");

    // Set up a multi-version cluster w/ replica sets

    st = new ShardingTest({
        shards: 2,
        mongos: 2,
        other: {
            // Replica set shards
            rs: true,
            mongosOptions: {binVersion: versionsToCheckMongos},
            configOptions: {binVersion: versionsToCheck},
            rsOptions: {binVersion: versionsToCheck, protocolVersion: 0},
            // TODO: SERVER-24163 remove after v3.4
            waitForCSRSSecondaries: false
        }
    });

    var nodesA = st.rs0.nodes;
    var nodesB = st.rs1.nodes;
    mongoses = [st.s0, st.s1];
    configs = [st.config0, st.config1, st.config2];

    var getVersion = function(mongo) {
        var result = mongo.getDB("admin").runCommand({serverStatus: 1});
        return result.version;
    };

    // Make sure we have hosts of all the different versions
    versionsFound = [];
    for (var j = 0; j < nodesA.length; j++)
        versionsFound.push(nodesA[j].getBinVersion());

    assert.allBinVersions(versionsToCheck, versionsFound);

    versionsFound = [];
    for (var j = 0; j < nodesB.length; j++)
        versionsFound.push(nodesB[j].getBinVersion());

    assert.allBinVersions(versionsToCheck, versionsFound);

    versionsFound = [];
    for (var j = 0; j < mongoses.length; j++)
        versionsFound.push(mongoses[j].getBinVersion());

    assert.allBinVersions(versionsToCheckMongos, versionsFound);

    versionsFound = [];
    for (var j = 0; j < configs.length; j++)
        versionsFound.push(configs[j].getBinVersion());

    assert.allBinVersions(versionsToCheck, versionsFound);

    st.stop();
})();
