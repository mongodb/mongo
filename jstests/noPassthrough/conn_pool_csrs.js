load("jstests/libs/parallelTester.js");

/**
 * @tags: [requires_replication, requires_sharding]
 *
 * Test that servers can be configured with distinct size limits for
 * connection pools that are connected to a config server.
 *
 * Heavily modified from jstest/noPassthrough/set_step_params.js
 */

(function() {
"use strict";

const poolSizeLogId = 6265600;

const rsMin = 10;
const rsMax = 20;

const baselineParameters = {
    ShardingTaskExecutorPoolMinSize: rsMin,
    ShardingTaskExecutorPoolMaxSize: rsMax,
    ShardingTaskExecutorPoolMinSizeForConfigServers: 4,
    ShardingTaskExecutorPoolMaxSizeForConfigServers: 6,
};

const mongosParameters = Object.assign(
    {logComponentVerbosity: tojson({network: {connectionPool: 5}})}, baselineParameters);

const st = new ShardingTest({
    config: {nodes: 1},
    shards: 1,
    rs0: {nodes: 1},
    mongos: [{setParameter: mongosParameters}],
});
const mongos = st.s0;
const configServer = st.c0;
const mongod = st.rs0.getPrimary();

const adminCmd = req => assert.commandWorked(mongos.adminCommand(req));

const populateTestDb = () => {
    const db = mongos.getDB('test');
    const coll = db.test;
    assert.commandWorked(coll.insert({x: 1}));
    assert.commandWorked(coll.insert({x: 2}));
    assert.commandWorked(coll.insert({x: 3}));
};

const setCSPoolBounds = (min, max) => {
    adminCmd(Object.assign({"setParameter": 1}, {
        ShardingTaskExecutorPoolMinSizeForConfigServers: min,
        ShardingTaskExecutorPoolMaxSizeForConfigServers: max,
    }));
};

// Make mongos open a connection to the config server. This is done by issuing a
// query that mongos will connect to the config server to execute.
const connectConfigServer = () => {
    jsTestLog('Cause Config Server connection');
    assert.commandWorked(mongos.getDB('config').runCommand(
        {find: "databases", limit: 1, "$readPreference": {mode: 'primary'}}));
};

// The (only) mongod has a 'test' database.
const connectMongodServer = () => {
    jsTestLog('Cause Mongod Server connection');
    assert.commandWorked(mongos.getDB('test').runCommand(
        {find: "test", limit: 1, "$readPreference": {mode: 'primary'}}));
};

// Waits until mongos emits a log line indicating that the conn pool
// `targetHost` is being resized. Returns all such lines.
const awaitUpdateHost = targetHost => {
    let hits;
    assert.soon(() => {
        let log = checkLog.getGlobalLog(st.s);
        // jsTestLog(`Fetched log: """${log}"""`);
        hits = checkLog.getGlobalLog(mongos)
                   .map(line => JSON.parse(line))
                   .filter(o => o.id == poolSizeLogId)
                   .filter(o => o.attr.host == targetHost);
        return hits.length > 0;
    }, `log lines id:${poolSizeLogId} for target ${targetHost}`, 10 * 1000, 1 * 1000, {
        runHangAnalyzer: false
    });
    return hits;
};

populateTestDb();

// Try a few {min,max} pairs.
for (const [min, max] of [[4, 6], [10, 20], [2, 4], [-1, -1]]) {
    jsTestLog(`Try ConfigServer pool bounds [${min},${max}]`);
    setCSPoolBounds(min, max);

    adminCmd({dropConnections: 1, hostAndPort: [configServer.host]});
    adminCmd({clearLog: 'global'});
    connectConfigServer();
    for (let o of awaitUpdateHost(configServer.host)) {
        const cascade = (x, fallback) => x >= 0 ? x : fallback;
        assert.eq(o.attr.minConns, cascade(min, rsMin));
        assert.eq(o.attr.maxConns, cascade(max, rsMax));
    }

    // Make sure the setting doesn't affect non-ConfigServer pools.
    adminCmd({dropConnections: 1, hostAndPort: [mongod.host]});
    adminCmd({clearLog: 'global'});
    connectMongodServer();
    for (let o of awaitUpdateHost(mongod.host)) {
        assert.eq(o.attr.minConns, rsMin);
        assert.eq(o.attr.maxConns, rsMax);
    }

    adminCmd(Object.assign({"setParameter": 1}, baselineParameters));
}

st.stop();
})();
