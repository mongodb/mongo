//
// Tests launching multi-version ShardingTest clusters.
//
//

load('./jstests/multiVersion/libs/verify_versions.js');

(function() {
"use strict";

function checkEquivalent(testConfig, st) {
    var expectedVersions = [testConfig.other.mongosOptions.binVersion];
    var expectedNodes = [...testConfig.shards.rs0.nodes, ...testConfig.shards.rs1.nodes];
    for (j = 0; j < expectedNodes.length; j++) {
        expectedVersions.push(expectedNodes[j].binVersion);
    }

    var versionsFound = [st.s0.getBinVersion()];
    var nodes = [...st._rs[0].test.nodes, ...st._rs[1].test.nodes];
    for (var j = 0; j < nodes.length; j++) {
        versionsFound.push(nodes[j].getBinVersion());
    }
    assert.allBinVersions(expectedVersions, versionsFound);
}

if (MongoRunner.areBinVersionsTheSame("last-continuous", "last-lts")) {
    jsTest.log("Skipping test because 'last-continuous' == 'last-lts'");
    return;
}

const invalidMixedVersionsToCheck = [
    {
        shards: {
            rs0: {nodes: [{binVersion: "last-continuous"}, {binVersion: "last-lts"}]},
            rs1: {nodes: [{binVersion: "last-lts"}]}
        },
        other: {mongosOptions: {binVersion: "last-lts"}}
    },
];

for (let config of invalidMixedVersionsToCheck) {
    jsTest.log("Testing invalid mixed versions: " + tojson(config));

    let err = assert.throws(
        () => new ShardingTest({shouldFailInit: true, shards: config.shards, other: config.other}));
    assert.eq(
        true,
        err.message.includes(
            "Can only specify one of 'last-lts' and 'last-continuous' in binVersion, not both."),
        "Unexpected Error");
}

const validMixedVersionsToCheck = [
    {
        shards: {rs0: {nodes: [{binVersion: "latest"}]}, rs1: {nodes: [{binVersion: "last-lts"}]}},
        other: {mongosOptions: {binVersion: "last-lts"}}
    },
];

for (let config of validMixedVersionsToCheck) {
    jsTest.log("Testing valid mixed versions: " + tojson(config));
    var st = new ShardingTest({shards: config.shards, other: config.other});
    var configs = [st.config0, st.config1, st.config2];
    checkEquivalent(config, st);

    st.stop();
}
})();
