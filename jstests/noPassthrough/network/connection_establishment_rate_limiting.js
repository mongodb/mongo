/**
 * Tests for the ingressConnectionEstablishment rate limiter.
 *
 * The ip-based exemptions tests are complicated by the gRPC testing logic, and so it is
 * excluded for now.
 * @tags: [
 *      grpc_incompatible,
 * ]
 */

import {get_ipaddr} from "jstests/libs/host_ipaddr.js";
import {
    isLinux,
} from "jstests/libs/os_helpers.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const runTestStandaloneParamsSetAtStartup = (setParams, testCase) => {
    let mongod = MongoRunner.runMongod({
        setParameter: {
            ...setParams,
            featureFlagRateLimitIngressConnectionEstablishment: true,
        }
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

const runTestStandaloneParamsSetAtRuntime = (setParams, testCase) => {
    let mongod = MongoRunner.runMongod(
        {setParameter: {featureFlagRateLimitIngressConnectionEstablishment: true}});
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

const runTestReplSet = (setParams, testCase) => {
    let replSet = new ReplSetTest({
        nodes: 1,
        nodeOptions: {
            setParameter: {
                ...setParams,
                featureFlagRateLimitIngressConnectionEstablishment: true,
            }
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
                }
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
        ingressConnectionEstablishmentRatePerSec: 10.0,
        ingressConnectionEstablishmentBurstSize: 500.0,
    });

    st.stop();
};

const testKillOnClientDisconnect = (conn) => {
    // TODO SERVER-104413: Assert on queue sizes.

    // The refreshRate is 1 conn/second, and so these connection attempts will outpace the
    // refreshRate and fail due to the socket timeout.
    assert.soon(() => {
        try {
            new Mongo(`mongodb://${conn.host}/?socketTimeoutMS=500`);
        } catch (e) {
            jsTestLog(e);
            return e.message.includes("Socket operation timed out");
        }

        return false;
    });

    assert.soon(() => checkLog.checkContainsOnceJson(
                    conn, 20883));  // Interrupted operation as its client disconnected
};

const testExemptIPsFromRateLimit = (conn) => {
    // TODO SERVER-104811: Assert on connections metrics.
    const ip = get_ipaddr();

    // Make one connection over the public ip that will consume a token.
    assert(new Mongo(`mongodb://${ip}:${conn.port}`));

    // The refreshRate is 1 conn/second, and so these connection attempts will outpace the
    // refreshRate and fail because queue depth is 0.
    assert.soon(() => {
        try {
            new Mongo(`mongodb://${ip}:${conn.port}`);
        } catch (e) {
            jsTestLog(e);
            return e.message.includes("Connection closed by peer") ||
                e.message.includes("Connection reset by peer") ||
                e.message.includes("established connection was aborted");
        }

        return false;
    });

    // A connection over an exempted ip will succeed.
    assert(new Mongo(`mongodb://127.0.0.1:${conn.port}`));
};

// The isConnected check will succeed if using the default baton because there is still data on
// the socket to read, and the markKillOnClientDisconnect logic with the default baton checks
// that rather than polling the socket state. Because of this, we don't run the
// killOnClientDisconnect test on non-Linux platforms.
if (isLinux()) {
    const testKillOnClientDisconnectOpts = {
        ingressConnectionEstablishmentRatePerSec: 1,
        ingressConnectionEstablishmentBurstSize: 1,
        ingressConnectionEstablishmentMaxQueueDepth: 10,
    };
    runTestStandaloneParamsSetAtStartup(testKillOnClientDisconnectOpts, testKillOnClientDisconnect);
    runTestStandaloneParamsSetAtRuntime(testKillOnClientDisconnectOpts, testKillOnClientDisconnect);
    runTestReplSet(testKillOnClientDisconnectOpts, testKillOnClientDisconnect);
    runTestShardedCluster(testKillOnClientDisconnectOpts, testKillOnClientDisconnect);
}

const testExemptIPsFromRateLimitOpts = {
    ingressConnectionEstablishmentRatePerSec: 1,
    ingressConnectionEstablishmentBurstSize: 1,
    ingressConnectionEstablishmentMaxQueueDepth: 0,
    maxEstablishingConnectionsOverride: {ranges: ["127.0.0.1"]},
};
runTestStandaloneParamsSetAtStartup(testExemptIPsFromRateLimitOpts, testExemptIPsFromRateLimit);
runTestStandaloneParamsSetAtRuntime(testExemptIPsFromRateLimitOpts, testExemptIPsFromRateLimit);
runTestReplSet(testExemptIPsFromRateLimitOpts, testExemptIPsFromRateLimit);
runTestShardedCluster(testExemptIPsFromRateLimitOpts, testExemptIPsFromRateLimit);
