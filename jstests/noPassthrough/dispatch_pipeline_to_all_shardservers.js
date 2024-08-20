// Various tests of the ability to establish a cursor on each mongod in a sharded cluster.

import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

function listMongodStats(db) {
    return db.getSiblingDB("admin").aggregate([{$_internalShardServerInfo: {}}]).toArray();
}

/**
 * Test that mongos can establish cursors on all nodes within a sharded cluster.
 */
(function() {
function runTest({shards, nodes}) {
    const st = new ShardingTest({
        mongos: 1,
        shards,
        config: 1,
        rs: {nodes},
    });
    const db = st.s.getDB(jsTestName());
    const results = listMongodStats(db);

    // Assert there are results from all hosts.
    const totalHosts = shards * nodes;
    assert.eq(totalHosts, results.length);
    st.stop();
}

runTest({shards: 1, nodes: 2});
runTest({shards: 1, nodes: 1});
runTest({shards: 2, nodes: 1});
runTest({shards: 2, nodes: 2});
runTest({shards: 2, nodes: 3});
runTest({shards: 3, nodes: 2});
}());

/**
 * Test that remote cursors are closed on all nodes when there is an error on one or more nodes.
 */
(function() {
const st = new ShardingTest({
    mongos: 1,
    shards: 1,
    config: 1,
    rs: {nodes: 2},
});
const db = st.s.getDB(jsTestName());
const primary = st.rs0.nodes[0];
const secondary = st.rs0.nodes[1];

assert.commandWorked(primary.adminCommand({
    configureFailPoint: 'failCommand',
    mode: 'alwaysOn',
    data: {
        errorCode: ErrorCodes.FailPointEnabled,
        failCommands: ["aggregate"],
        failInternalCommands: true,
    }
}));
assert.commandFailed(db.adminCommand({
    aggregate: 1,
    pipeline: [
        {$_internalShardServerInfo: {}},
    ],
    cursor: {batchSize: 1},
}));
primary.adminCommand({configureFailPoint: 'failCommand', mode: 'off'});

assert.soon(() => {
    const primaryCursors =
        primary.getDB("admin")
            .aggregate([{$currentOp: {idleCursors: true}}, {$match: {type: "idleCursor"}}])
            .toArray();
    const secondaryCursors =
        secondary.getDB("admin")
            .aggregate([{$currentOp: {idleCursors: true}}, {$match: {type: "idleCursor"}}])
            .toArray();

    try {
        assert.eq(0, primaryCursors.length, tojson(primaryCursors));
        assert.eq(0, secondaryCursors.length, tojson(secondaryCursors));
    } catch (err) {
        jsTestLog(tojson(err));
        return false;
    }

    return true;
});

st.stop();
}());

/**
 * Test that we can gracefully handle an imbalanced topology where some shards have fewer replica
 * set members than others (SERVER-79372).
 */
(function() {
"use strict";
const st = new ShardingTest({
    mongos: 1,
    // different numbers of mongods between shards.
    shards: {rs0: {nodes: 1}, rs1: {nodes: 2}},
    config: 1,  // not relevant for this test.
});

// This once tripped an invariant failure: SERVER-79372.
const results = listMongodStats(st.s.getDB(jsTestName()));

// Assert there are results from all hosts.
const totalHosts = 3;
assert.eq(totalHosts, results.length, results);

st.stop();
}());

/**
 * Same sort of test (SERVER-79372) but now where the config server has a different number of
 * shards, and it gets migrated from a dedicated config server to be one of the shards. This is how
 * the bug was originally discovered.
 */
(function() {
"use strict";
const st = new ShardingTest({
    mongos: 1,
    shards: {rs0: {nodes: 2}, rs1: {nodes: 2}},
    config: {rs: {nodes: 1}},
});
// This one has always worked fine.
let results = listMongodStats(st.s.getDB(jsTestName()));
assert.eq(4, results.length, results);

if (FeatureFlagUtil.isPresentAndEnabled(st.s0, "FeatureFlagTransitionToCatalogShard")) {
    assert.commandWorked(st.s.getDB("admin").runCommand({transitionFromDedicatedConfigServer: 1}));
    // After the above command, this once tripped an invariant failure: SERVER-79372.
    results = listMongodStats(st.s.getDB(jsTestName()));

    // Assert there are results from all hosts.
    assert.eq(5, results.length, results);
}

st.stop();
}());
