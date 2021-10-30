//
// Tests launching multi-version ShardingTest clusters.
//
//

load('./jstests/multiVersion/libs/verify_versions.js');

(function() {
"use strict";

// TODO(SERVER-61100): Re-enable this test.
if (true) {
    jsTestLog("Skipping test as it is currently disabled.");
    return;
}

// Sharded cluster upgrade order: config servers -> shards -> mongos.
const mixedVersionsToCheck = [
    {config: ["latest"], shard: ["last-lts", "latest"], mongos: ["last-lts"]},
    {config: ["latest"], shard: ["last-continuous", "latest"], mongos: ["last-continuous"]},
    {config: ["latest"], shard: ["last-continuous", "last-lts"], mongos: ["last-continuous"]},
    {config: ["latest"], shard: ["last-lts", "last-continuous"], mongos: ["last-lts"]},
];

for (let versions of mixedVersionsToCheck) {
    jsTest.log("Testing mixed versions: " + tojson(versions));
    try {
        // Set up a multi-version cluster
        var st = new ShardingTest({
            shards: 2,
            mongos: 2,
            other: {
                mongosOptions: {binVersion: versions.mongos},
                configOptions: {binVersion: versions.config},
                shardOptions: {binVersion: versions.shard},
                enableBalancer: true
            }
        });
    } catch (e) {
        if (e instanceof Error) {
            if (e.message.includes(
                    "Can only specify one of 'last-lts' and 'last-continuous' in binVersion, not both.")) {
                continue;
            }
        }
        throw e;
    }
    if ((versions.shard[0] === "last-continuous" && versions.shard[1] === "last-lts") ||
        (versions.shard[1] === "last-continuous" && versions.shard[0] === "last-lts")) {
        assert(
            MongoRunner.areBinVersionsTheSame("last-continuous", "last-lts"),
            "Should have thrown error in creating ShardingTest because can only specify one of 'last-lts' and 'last-continuous' in binVersion, not both.");
    }
    var shards = [st.shard0, st.shard1];
    var mongoses = [st.s0, st.s1];
    var configs = [st.config0, st.config1, st.config2];

    // Make sure we have hosts of all the different versions
    var versionsFound = [];
    for (var j = 0; j < shards.length; j++)
        versionsFound.push(shards[j].getBinVersion());

    assert.allBinVersions(versions.shard, versionsFound);

    versionsFound = [];
    for (var j = 0; j < mongoses.length; j++)
        versionsFound.push(mongoses[j].getBinVersion());

    assert.allBinVersions(versions.mongos, versionsFound);

    versionsFound = [];
    for (var j = 0; j < configs.length; j++)
        versionsFound.push(configs[j].getBinVersion());

    assert.allBinVersions(versions.config, versionsFound);

    st.stop();
}
})();
