/**
 * Test count commands against the config servers, including when some of them are down.
 * This test fails when run with authentication due to SERVER-6327
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";

let st = new ShardingTest({name: "sync_conn_cmd", shards: TestData.configShard ? 1 : 0, config: 3});
st.s.setSecondaryOk();

let configDB = st.config;
let coll = configDB.test;

for (let x = 0; x < 10; x++) {
    assert.commandWorked(coll.insert({v: x}));
}

if (st.configRS) {
    // Make sure the inserts are replicated to all config servers.
    st.configRS.awaitReplication();
}

let testNormalCount = function () {
    let cmdRes = configDB.runCommand({count: coll.getName()});
    assert(cmdRes.ok);
    assert.eq(10, cmdRes.n);
};

let testCountWithQuery = function () {
    let cmdRes = configDB.runCommand({count: coll.getName(), query: {v: {$gt: 6}}});
    assert(cmdRes.ok);
    assert.eq(3, cmdRes.n);
};

// Use invalid query operator to make the count return error
let testInvalidCount = function () {
    let cmdRes = configDB.runCommand({count: coll.getName(), query: {$c: {$abc: 3}}});
    assert(!cmdRes.ok);
    assert(cmdRes.errmsg.length > 0);
};

// Test with all config servers up
testNormalCount();
testCountWithQuery();
testInvalidCount();

// Test with the first config server down
st.configRS.stop(0, undefined /* signal */, undefined /* opts */, {forRestart: true});

testNormalCount();
testCountWithQuery();
testInvalidCount();

// Test with the first and second config server down
st.configRS.stop(1, undefined /* signal */, undefined /* opts */, {forRestart: true});
jsTest.log("Second server is down");

testNormalCount();
testCountWithQuery();
testInvalidCount();

// Restart the config servers to ensure teardown checks can execute correctly. Shut down the final
// node so we can use the canonical helper for restarting the entire CSRS.
st.configRS.stop(2, undefined /* signal */, undefined /* opts */, {forRestart: true});
st.restartAllConfigServers();

st.stop();
