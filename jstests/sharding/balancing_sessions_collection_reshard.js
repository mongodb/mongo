/*
 * Tests that the balancer does not split the chunks for the sessions collection
 * if the shard key is not _id.
 */
(function() {
"use strict";

let numShards = 2;
const kMinNumChunks = 2;
const clusterName = jsTest.name();
const st = new ShardingTest({
    name: clusterName,
    shards: numShards,
    other: {configOptions: {setParameter: {minNumChunksForSessionsCollection: kMinNumChunks}}}
});

let waitForBalancerToRun = function() {
    let lastRoundNumber =
        assert.commandWorked(st.s.adminCommand({balancerStatus: 1})).numBalancerRounds;
    st.startBalancer();

    assert.soon(function() {
        let res = assert.commandWorked(st.s.adminCommand({balancerStatus: 1}));
        return res.mode == "full" && res.numBalancerRounds - lastRoundNumber > 1;
    });

    st.stopBalancer();
};

const kSessionsNs = "config.system.sessions";
let configDB = st.s.getDB("config");

jsTest.log("Verify that the sessions collection is successfully dropped.");

assert.commandWorked(configDB.runCommand({drop: "system.sessions"}));

jsTest.log("Verify that the sessions collection is successfully recreated and resharded.");

assert.commandWorked(st.s.adminCommand({enableSharding: "config"}));
assert.commandWorked(st.s.adminCommand({shardCollection: kSessionsNs, key: {oldRoles: 1}}));

jsTest.log("Verify that balancer does not fail after resharding.");
waitForBalancerToRun();

jsTest.log(
    "Verify that there is a single chunk for the collection and the bounds are the resharded key.");
assert.eq(1, configDB.chunks.count({ns: kSessionsNs}));

let doc = configDB.chunks.findOne({ns: kSessionsNs});
assert(doc.min.hasOwnProperty("oldRoles"));
assert(doc.max.hasOwnProperty("oldRoles"));

st.stop();
}());
