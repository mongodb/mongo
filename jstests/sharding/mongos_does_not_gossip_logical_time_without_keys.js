/**
 * Tests that mongos does not gossip cluster time metadata until at least one key is created on the
 * config server, and that it does not block waiting for keys at startup.
 */

(function() {
"use strict";

load("jstests/multiVersion/libs/multi_rs.js");
load("jstests/multiVersion/libs/multi_cluster.js");  // For restartMongoses.

function assertContainsValidLogicalTime(res, check) {
    assert.hasFields(res, ["$clusterTime"]);
    assert.hasFields(res.$clusterTime, ["signature", "clusterTime"]);
    // clusterTime must be greater than the uninitialzed value.
    // TODO: SERVER-31986 this check can be done only for authenticated connections that do not
    // have advance_cluster_time privilege.
    if (check) {
        assert.eq(bsonWoCompare(res.$clusterTime.clusterTime, Timestamp(0, 0)), 1);
    }
    assert.hasFields(res.$clusterTime.signature, ["hash", "keyId"]);
    // The signature must have been signed by a key with a valid generation.
    if (check) {
        assert(res.$clusterTime.signature.keyId > NumberLong(0));
    }
}

let st = new ShardingTest({shards: {rs0: {nodes: 2}}});

// Verify there are keys in the config server eventually, since mongos doesn't block for keys at
// startup, and that once there are, mongos sends $clusterTime with a signature in responses.
assert.soonNoExcept(function() {
    assert(st.s.getDB("admin").system.keys.count() >= 2);

    let res = assert.commandWorked(st.s.getDB("test").runCommand({hello: 1}));
    assertContainsValidLogicalTime(res, false);

    return true;
}, "expected keys to be created and for mongos to send signed cluster times");

// Enable the failpoint, remove all keys, and restart the config servers with the failpoint
// still enabled to guarantee there are no keys.
for (let i = 0; i < st.configRS.nodes.length; i++) {
    assert.commandWorked(st.configRS.nodes[i].adminCommand(
        {"configureFailPoint": "disableKeyGeneration", "mode": "alwaysOn"}));
}
let res = st.configRS.getPrimary().getDB("admin").system.keys.remove({purpose: "HMAC"});
assert(res.nRemoved >= 2);
assert(st.s.getDB("admin").system.keys.count() == 0,
       "expected there to be no keys on the config server");
st.configRS.stopSet(null /* signal */, true /* forRestart */);
st.configRS.startSet(
    {restart: true, setParameter: {"failpoint.disableKeyGeneration": "{'mode':'alwaysOn'}"}});

// Limit the max time between refreshes on the config server, so new keys are created quickly.
st.configRS.getPrimary().adminCommand({
    "configureFailPoint": "maxKeyRefreshWaitTimeOverrideMS",
    "mode": "alwaysOn",
    "data": {"overrideMS": 1000}
});

// Disable the failpoint.
for (let i = 0; i < st.configRS.nodes.length; i++) {
    assert.commandWorked(st.configRS.nodes[i].adminCommand(
        {"configureFailPoint": "disableKeyGeneration", "mode": "off"}));
}

// Mongos should restart with no problems.
st.restartMongoses();

// Eventually mongos will discover the new keys, and start signing cluster times.
assert.soonNoExcept(function() {
    assertContainsValidLogicalTime(st.s.getDB("test").runCommand({hello: 1}), false);
    return true;
}, "expected mongos to eventually start signing cluster times", 60 * 1000);  // 60 seconds.

// There may be a delay between the creation of the first and second keys, but mongos will start
// signing after seeing the first key, so there is only guaranteed to be one at this point.
assert(st.s.getDB("admin").system.keys.count() >= 1,
       "expected there to be at least one generation of keys on the config server");

st.stop();
})();
