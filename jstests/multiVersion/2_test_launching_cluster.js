//
// Tests launching multi-version ShardingTest clusters.
//

load('./jstests/multiVersion/libs/verify_versions.js');

(function() {
"use strict";
// Check our latest versions
var versionsToCheck = ["last-lts", "latest"];
var versionsToCheckConfig = ["latest"];
var versionsToCheckMongos = ["last-lts"];

jsTest.log("Testing mixed versions...");

// Set up a multi-version cluster
var st = new ShardingTest({
    shards: 2,
    mongos: 2,
    other: {
        mongosOptions: {binVersion: versionsToCheckMongos},
        configOptions: {binVersion: versionsToCheckConfig},
        shardOptions: {binVersion: versionsToCheck},
        enableBalancer: true
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

assert.allBinVersions(versionsToCheckConfig, versionsFound);

st.stop();
})();
