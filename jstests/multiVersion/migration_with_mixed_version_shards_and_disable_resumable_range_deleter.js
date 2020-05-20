/*
 * Tests that migrations behave correctly between v4.4 and v4.6 when one or both nodes have the
 * 'disableResumableRangeDeleter' parameter set to true.
 *
 * requires_persistence because this test restarts shards and expects them to have their data files.
 * @tags: [requires_persistence]
 */

(function() {
"use strict";

const dbName = "test";

function getNewNs(dbName) {
    if (typeof getNewNs.counter == 'undefined') {
        getNewNs.counter = 0;
    }
    getNewNs.counter++;
    const collName = "ns" + getNewNs.counter;
    return [collName, dbName + "." + collName];
}

function setDisableResumableRangeDeleter(value, rs) {
    const getParameterRes =
        rs.getPrimary().adminCommand({getParameter: 1, disableResumableRangeDeleter: 1});
    assert.commandWorked(getParameterRes);
    if (getParameterRes.disableResumableRangeDeleter == value) {
        return;
    }
    rs.stopSet(null /* signal */, true /* forRestart */);
    rs.startSet({restart: true, setParameter: {disableResumableRangeDeleter: value}});
}

const st = new ShardingTest({
    shards: {rs0: {nodes: [{binVersion: "latest"}]}, rs1: {nodes: [{binVersion: "last-stable"}]}},
    other: {mongosOptions: {binVersion: "last-stable"}}
});
assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
assert.commandWorked(st.s.adminCommand({movePrimary: dbName, to: st.shard0.shardName}));

const v46shard = st.rs0;
const v44shard = st.rs1;

//
// Tests with v4.6 donor, v4.4 recipient
//

(() => {
    jsTestLog("v4.6 donor, v4.4 recipient, both disableResumableRangeDeleter=false");
    setDisableResumableRangeDeleter(false, v46shard);
    setDisableResumableRangeDeleter(false, v44shard);
    const [collName, ns] = getNewNs(dbName);
    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {x: 1}}));
    assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {x: 0}, to: v44shard.name}));
})();

(() => {
    jsTestLog(
        "v4.6 donor with disableResumableRangeDeleter=true, v4.4 recipient with disableResumableRangeDeleter=false");
    setDisableResumableRangeDeleter(true, v46shard);
    setDisableResumableRangeDeleter(false, v44shard);
    const [collName, ns] = getNewNs(dbName);
    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {x: 1}}));
    assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {x: 0}, to: v44shard.name}));
})();

(() => {
    jsTestLog("v4.6 donor, v4.4 recipient, both disableResumableRangeDeleter=true");
    setDisableResumableRangeDeleter(true, v46shard);
    setDisableResumableRangeDeleter(true, v44shard);
    const [collName, ns] = getNewNs(dbName);
    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {x: 1}}));
    assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {x: 0}, to: v44shard.name}));
})();

(() => {
    jsTestLog(
        "v4.6 donor with disableResumableRangeDeleter=false, v4.4 recipient with disableResumableRangeDeleter=true");
    setDisableResumableRangeDeleter(false, v46shard);
    setDisableResumableRangeDeleter(true, v44shard);
    const [collName, ns] = getNewNs(dbName);
    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {x: 1}}));
    assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {x: 0}, to: v44shard.name}));
})();

//
// Tests with v4.4 donor, v4.6 recipient
//

assert.commandWorked(st.s.adminCommand({movePrimary: dbName, to: st.shard1.shardName}));

(() => {
    jsTestLog("v4.4 donor, v4.6 recipient, both disableResumableRangeDeleter=false");
    setDisableResumableRangeDeleter(false, v44shard);
    setDisableResumableRangeDeleter(false, v46shard);
    const [collName, ns] = getNewNs(dbName);
    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {x: 1}}));
    assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {x: 0}, to: v46shard.name}));
})();

(() => {
    jsTestLog(
        "v4.4 donor with disableResumableRangeDeleter=true, v4.6 recipient with disableResumableRangeDeleter=false");
    setDisableResumableRangeDeleter(true, v44shard);
    setDisableResumableRangeDeleter(false, v46shard);
    const [collName, ns] = getNewNs(dbName);
    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {x: 1}}));
    assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {x: 0}, to: v46shard.name}));
})();

(() => {
    jsTestLog("v4.4 donor, v4.6 recipient, both disableResumableRangeDeleter=true");
    setDisableResumableRangeDeleter(true, v44shard);
    setDisableResumableRangeDeleter(true, v46shard);
    const [collName, ns] = getNewNs(dbName);
    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {x: 1}}));
    assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {x: 0}, to: v46shard.name}));
})();

(() => {
    jsTestLog(
        "v4.4 donor with disableResumableRangeDeleter=false, v4.6 recipient with disableResumableRangeDeleter=true");
    setDisableResumableRangeDeleter(false, v44shard);
    setDisableResumableRangeDeleter(true, v46shard);
    const [collName, ns] = getNewNs(dbName);
    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {x: 1}}));
    assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {x: 0}, to: v46shard.name}));
})();

st.stop();
})();
