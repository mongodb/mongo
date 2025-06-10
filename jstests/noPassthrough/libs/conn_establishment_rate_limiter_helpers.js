const getLimiterStats = (conn, {log = false}) => {
    const serverStatus = conn.adminCommand({serverStatus: 1});
    assert(serverStatus, "Failed to get server status");
    const result = {
        connections: serverStatus.connections,
        ingressSessionEstablishmentQueues: serverStatus.queues.ingressSessionEstablishment
    };
    if (log) {
        jsTestLog("Limiter stats: " + tojson(result));
    }
    return result;
};

const getConnectionStats = (conn) => {
    const {connections} = getLimiterStats(conn, {log: true});
    assert(connections, "Failed to get connection stats");
    return connections;
};

const runTestStandaloneParamsSetAtStartup = (setParams, testCase) => {
    let mongod = MongoRunner.runMongod({
        setParameter: {
            ...setParams,
            featureFlagRateLimitIngressConnectionEstablishment: true,
        },
        config: "jstests/noPassthrough/libs/net.max_incoming_connections_rate_limiter.yaml",
    });

    testCase(mongod);

    // Let connections through again.
    mongod.adminCommand({
        setParameter: 1,
        ingressConnectionEstablishmentRatePerSec: 10.0,
        ingressConnectionEstablishmentBurstCapacitySecs: 500.0,
    });

    MongoRunner.stopMongod(mongod);
};

const runTestStandaloneParamsSetAtRuntime = (setParams, testCase) => {
    let mongod = MongoRunner.runMongod({
        setParameter: {featureFlagRateLimitIngressConnectionEstablishment: true},
        config: "jstests/noPassthrough/libs/net.max_incoming_connections_rate_limiter.yaml",
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
        ingressConnectionEstablishmentBurstCapacitySecs: 500.0,
    });

    MongoRunner.stopMongod(mongod);
};

const runTestReplSet = (setParams, testCase) => {
    let replSet = new ReplSetTest({
        nodes: 1,
        nodeOptions: {
            setParameter: {
                ...setParams,
                featureFlagRateLimitIngressConnectionEstablishment: true,
            },
            config: "jstests/noPassthrough/libs/net.max_incoming_connections_rate_limiter.yaml",
        }
    });
    replSet.startSet();
    replSet.initiate();

    testCase(replSet.getPrimary());

    // Let connections through again.
    replSet.getPrimary().adminCommand({
        setParameter: 1,
        ingressConnectionEstablishmentRatePerSec: 10.0,
        ingressConnectionEstablishmentBurstCapacitySecs: 500.0,
    });

    replSet.stopSet();
};

const runTestShardedCluster = (setParams, testCase) => {
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
                config: "jstests/noPassthrough/libs/net.max_incoming_connections_rate_limiter.yaml",
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
        ingressConnectionEstablishmentBurstCapacitySecs: 500.0,
        ingressConnectionEstablishmentMaxQueueDepth: 100,
    });

    st.stop();
};
