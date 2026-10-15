/**
 * @tags: [requires_replication, requires_sharding]
 *
 * Test that servers can be configured with distinct size limits for connection pools connected to
 * a config server, including the separate bounds and connecting limit for the Sharding-Fixed pool.
 *
 * Heavily modified from jstest/noPassthrough/set_step_params.js
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const poolSizeLogId = 6265600;
const fixedPoolClampLogId = 10711800;
const arbitraryPoolPattern = /^NetworkInterfaceTL-TaskExecutorPool-/;
const fixedPoolName = "NetworkInterfaceTL-Sharding-Fixed";
const fixedPoolPattern = new RegExp(`^${fixedPoolName}$`);

const rsMin = 10;
const rsMax = 20;
const fixedPoolDefaultMin = 1;
const fixedPoolDefaultMax = 32767;
const fixedPoolDefaultMaxConnecting = 2;

const baselineParameters = {
    ShardingTaskExecutorPoolMinSize: rsMin,
    ShardingTaskExecutorPoolMaxSize: rsMax,
    ShardingTaskExecutorPoolMinSizeForConfigServers: 4,
    ShardingTaskExecutorPoolMaxSizeForConfigServers: 6,
};

const mongosParameters = Object.assign(
    {logComponentVerbosity: tojson({network: {connectionPool: 5}})},
    baselineParameters,
);

const st = new ShardingTest({
    config: {nodes: 1},
    shards: 1,
    rs0: {nodes: 1},
    mongos: [{setParameter: mongosParameters}],
});
const mongos = st.s0;
const configServer = st.c0;
const mongod = st.rs0.getPrimary();

const adminCmd = (req) => assert.commandWorked(mongos.adminCommand(req));

const populateTestDb = () => {
    const db = mongos.getDB("test");
    const coll = db.test;
    assert.commandWorked(coll.insert({x: 1}));
    assert.commandWorked(coll.insert({x: 2}));
    assert.commandWorked(coll.insert({x: 3}));
};

const setCSPoolBounds = (min, max) => {
    adminCmd(
        Object.assign(
            {"setParameter": 1},
            {
                ShardingTaskExecutorPoolMinSizeForConfigServers: min,
                ShardingTaskExecutorPoolMaxSizeForConfigServers: max,
            },
        ),
    );
};

const setFixedPoolBounds = (min, max) => {
    adminCmd({
        setParameter: 1,
        ShardingFixedTaskExecutorPoolMinSize: min,
        ShardingFixedTaskExecutorPoolMaxSize: max,
    });
};

// Make mongos open a connection to the config server. This is done by issuing a
// query that mongos will connect to the config server to execute.
const connectConfigServer = () => {
    jsTestLog("Cause Config Server connection");
    assert.commandWorked(
        mongos
            .getDB("config")
            .runCommand({find: "databases", limit: 1, "$readPreference": {mode: "primary"}}),
    );
};

let nextFixedPoolDbId = 0;
const nextFixedPoolDbName = () => `fixedPoolTest${nextFixedPoolDbId++}`;
const connectFixedExecutorToConfigServer = () => {
    const dbName = nextFixedPoolDbName();
    jsTestLog("Cause Sharding-Fixed executor connection to config server");
    assert.commandWorked(mongos.adminCommand({enableSharding: dbName}));
};

// The (only) mongod has a 'test' database.
const connectMongodServer = () => {
    jsTestLog("Cause Mongod Server connection");
    assert.commandWorked(
        mongos
            .getDB("test")
            .runCommand({find: "test", limit: 1, "$readPreference": {mode: "primary"}}),
    );
};

// Wait for a resize log for `targetHost` from the selected connection pool.
const awaitUpdateHost = (targetHost, poolPattern) => {
    let hits;
    assert.soon(
        () => {
            hits = checkLog
                .getGlobalLog(mongos)
                .map((line) => JSON.parse(line))
                .filter((o) => o.id == poolSizeLogId)
                .filter((o) => poolPattern.test(o.attr.pool))
                .filter((o) => o.attr.host == targetHost);
            return hits.length > 0;
        },
        `log lines id:${poolSizeLogId} for target ${targetHost}`,
        10 * 1000,
        1 * 1000,
        {
            runHangAnalyzer: false,
        },
    );
    return hits;
};

const assertFixedPoolBounds = (min, max) => {
    adminCmd({dropConnections: 1, hostAndPort: [configServer.name]});
    adminCmd({clearLog: "global"});
    connectFixedExecutorToConfigServer();
    for (const entry of awaitUpdateHost(configServer.name, fixedPoolPattern)) {
        assert.docEq(
            {minConns: min, maxConns: max},
            {minConns: entry.attr.minConns, maxConns: entry.attr.maxConns},
            {entry},
        );
    }
};

populateTestDb();

jsTestLog("Check default and validation of Sharding-Fixed connecting limit");
assert.eq(
    fixedPoolDefaultMaxConnecting,
    adminCmd({getParameter: 1, ShardingFixedTaskExecutorPoolMaxConnecting: 1})
        .ShardingFixedTaskExecutorPoolMaxConnecting,
);
for (const invalidLimit of [0, -1]) {
    assert.commandFailedWithCode(
        mongos.adminCommand({
            setParameter: 1,
            ShardingFixedTaskExecutorPoolMaxConnecting: invalidLimit,
        }),
        ErrorCodes.BadValue,
    );
}

// Try a few {min,max} pairs.
for (const [min, max] of [
    [4, 6],
    [10, 20],
    [2, 4],
    [-1, -1],
]) {
    jsTestLog(`Try ConfigServer pool bounds [${min},${max}]`);
    setCSPoolBounds(min, max);

    adminCmd({dropConnections: 1, hostAndPort: [configServer.name]});
    adminCmd({clearLog: "global"});
    connectConfigServer();
    for (const o of awaitUpdateHost(configServer.name, arbitraryPoolPattern)) {
        const cascade = (x, fallback) => (x >= 0 ? x : fallback);
        assert.eq(o.attr.minConns, cascade(min, rsMin));
        assert.eq(o.attr.maxConns, cascade(max, rsMax));
    }

    // Make sure the setting doesn't affect non-ConfigServer pools.
    adminCmd({dropConnections: 1, hostAndPort: [mongod.name]});
    adminCmd({clearLog: "global"});
    connectMongodServer();
    for (const o of awaitUpdateHost(mongod.name, arbitraryPoolPattern)) {
        assert.eq(o.attr.minConns, rsMin);
        assert.eq(o.attr.maxConns, rsMax);
    }

    adminCmd(Object.assign({"setParameter": 1}, baselineParameters));
}

jsTestLog("Check default Sharding-Fixed pool bounds with tuned arbitrary pool bounds");
assertFixedPoolBounds(fixedPoolDefaultMin, fixedPoolDefaultMax);

for (const [min, max] of [
    [2, 3],
    [6, 8],
]) {
    jsTestLog(`Try Sharding-Fixed pool bounds [${min},${max}]`);
    setFixedPoolBounds(min, max);

    assertFixedPoolBounds(min, max);
}

jsTestLog("Clamp Sharding-Fixed pool bounds when minimum exceeds maximum");
setFixedPoolBounds(6, 3);
assert.soon(
    () =>
        checkLog
            .getGlobalLog(mongos)
            .map((line) => JSON.parse(line))
            .some((o) => o.id == fixedPoolClampLogId && o.s == "E"),
    "expected an error when Sharding-Fixed minimum exceeds maximum",
    10 * 1000,
    1 * 1000,
    {runHangAnalyzer: false},
);
const clampError = checkLog
    .getGlobalLog(mongos)
    .map((line) => JSON.parse(line))
    .find((o) => o.id == fixedPoolClampLogId && o.s == "E");
assert.docEq(
    {
        minParam: "ShardingFixedTaskExecutorPoolMinSize",
        minValue: 6,
        maxParam: "ShardingFixedTaskExecutorPoolMaxSize",
        maxValue: 3,
    },
    {
        minParam: clampError.attr.minParam,
        minValue: clampError.attr.minValue,
        maxParam: clampError.attr.maxParam,
        maxValue: clampError.attr.maxValue,
    },
    tojson(clampError),
);
assertFixedPoolBounds(3, 3);

jsTestLog("Lower Sharding-Fixed pool bounds together at runtime");
jsTestLog("The new max is below the previous minimum");
setFixedPoolBounds(1, 2);
assertFixedPoolBounds(1, 2);

const getFixedPoolStats = () =>
    adminCmd({connPoolStats: 1}).pools[fixedPoolName]?.[configServer.name];
// `predicate` is a function that takes the current fixed pool stats and returns true if the desired
// condition is met, and description describes the predicate for logging purposes.
const awaitFixedPoolStats = (predicate, description) => {
    let lastStats;
    assert.soon(
        () => {
            lastStats = getFixedPoolStats();
            return lastStats && predicate(lastStats);
        },
        () => `expected Sharding-Fixed ${description}; last stats: ${tojson(lastStats)}`,
    );
};

jsTestLog("Check independent Sharding-Fixed connecting limits at runtime");
const fixedMinConns = 6;
const fixedMaxConns = 8;
const arbitraryMaxConnecting = 7;
const initialFixedMaxConnecting = 2;
const increasedFixedMaxConnecting = 4;
const reducedMaxConnecting = 1;
setFixedPoolBounds(fixedMinConns, fixedMaxConns);
adminCmd({
    setParameter: 1,
    ShardingTaskExecutorPoolMaxConnecting: arbitraryMaxConnecting,
    ShardingFixedTaskExecutorPoolMaxConnecting: initialFixedMaxConnecting,
});
// Waiting in hello will prevent connections from completing.
const helloFailPoint = configureFailPoint(configServer, "shardWaitInHello");
const fixedExecutorRequest = new Thread(
    function (host, dbName) {
        const conn = new Mongo(host);
        assert.commandWorked(conn.adminCommand({enableSharding: dbName}));
    },
    mongos.host,
    nextFixedPoolDbName(),
);
try {
    adminCmd({dropConnections: 1, hostAndPort: [configServer.name]});
    fixedExecutorRequest.start();
    awaitFixedPoolStats(
        (stats) => stats.refreshing === initialFixedMaxConnecting,
        `${initialFixedMaxConnecting} pending connections`,
    );

    adminCmd({setParameter: 1, ShardingTaskExecutorPoolMaxConnecting: reducedMaxConnecting});

    adminCmd({
        setParameter: 1,
        ShardingFixedTaskExecutorPoolMaxConnecting: increasedFixedMaxConnecting,
    });
    awaitFixedPoolStats(
        (stats) => stats.refreshing === increasedFixedMaxConnecting,
        `${increasedFixedMaxConnecting} pending connections despite the lower arbitrary limit`,
    );

    adminCmd({setParameter: 1, ShardingFixedTaskExecutorPoolMaxConnecting: reducedMaxConnecting});
    assert.eq(
        increasedFixedMaxConnecting,
        getFixedPoolStats()?.refreshing,
        "Lowering the limit must not cancel existing connection attempts",
    );
} finally {
    helloFailPoint.off();
    fixedExecutorRequest.join();
}
awaitFixedPoolStats(
    (stats) => stats.refreshing === 0 && stats.available >= fixedMinConns,
    `no pending connections and at least ${fixedMinConns} available connections`,
);

st.stop();
