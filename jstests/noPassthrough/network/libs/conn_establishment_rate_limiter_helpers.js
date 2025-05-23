import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

export const getConnectionStats = (conn) => {
    const connStats = conn.adminCommand({serverStatus: 1})["connections"];
    assert.neq(null, connStats, "Failed to get connection stats");
    jsTestLog("Connection stats: " + tojson(connStats));
    return connStats;
};

export const runTestStandaloneParamsSetAtStartup = (setParams, testCase) => {
    let mongod = MongoRunner.runMongod({
        setParameter: {
            ...setParams,
            featureFlagRateLimitIngressConnectionEstablishment: true,
        },
        config: "jstests/noPassthrough/network/libs/net.max_incoming_connections_rate_limiter.yaml",
    });

    testCase(mongod);

    // Let connections through again.
    mongod.adminCommand({
        setParameter: 1,
        ingressConnectionEstablishmentRatePerSec: 10.0,
        ingressConnectionEstablishmentBurstSize: 500.0,
    });

    MongoRunner.stopMongod(mongod);
};

export const runTestStandaloneParamsSetAtRuntime = (setParams, testCase) => {
    let mongod = MongoRunner.runMongod({
        setParameter: {featureFlagRateLimitIngressConnectionEstablishment: true},
        config: "jstests/noPassthrough/network/libs/net.max_incoming_connections_rate_limiter.yaml",
    });
    mongod.adminCommand({
        setParameter: 1,
        ...setParams,
    });

    // Make one connection over localhost to ensure the first token has been consumed.
    assert(new Mongo(`mongodb://127.0.0.1:${mongod.port}`));

    testCase(mongod);

    // Let connections through again.
    mongod.adminCommand({
        setParameter: 1,
        ingressConnectionEstablishmentRatePerSec: 10.0,
        ingressConnectionEstablishmentBurstSize: 500.0,
    });

    MongoRunner.stopMongod(mongod);
};

export const runTestReplSet = (setParams, testCase) => {
    let replSet = new ReplSetTest({
        nodes: 1,
        nodeOptions: {
            setParameter: {
                ...setParams,
                featureFlagRateLimitIngressConnectionEstablishment: true,
            },
            config:
                "jstests/noPassthrough/network/libs/net.max_incoming_connections_rate_limiter.yaml",
        }
    });
    replSet.startSet();
    replSet.initiate();

    testCase(replSet.getPrimary());

    // Let connections through again.
    replSet.getPrimary().adminCommand({
        setParameter: 1,
        ingressConnectionEstablishmentRatePerSec: 10.0,
        ingressConnectionEstablishmentBurstSize: 500.0,
    });

    replSet.stopSet();
};

export const runTestShardedCluster = (setParams, testCase) => {
    let st = new ShardingTest({
        shards: 1,
        mongos: 1,
        config: 1,
        keyFile: 'jstests/libs/key1',
        useHostname: false,
        other: {
            mongosOptions: {
                setParameter: {
                    ...setParams,
                    featureFlagRateLimitIngressConnectionEstablishment: true,
                },
                config:
                    "jstests/noPassthrough/network/libs/net.max_incoming_connections_rate_limiter.yaml",
            }
        }
    });

    // The connection has to be authed to run checkLog.
    const admin = st.s0.getDB("admin");
    admin.createUser({user: 'admin', pwd: 'pass', roles: jsTest.adminUserRoles});
    assert(admin.auth('admin', 'pass'));

    testCase(st.s0);

    // Let connections through again.
    st.s0.adminCommand({
        setParameter: 1,
        ingressConnectionEstablishmentRatePerSec: 1000.0,
        ingressConnectionEstablishmentBurstSize: 500.0,
        ingressConnectionEstablishmentMaxQueueDepth: 100,
    });

    st.stop();
};
