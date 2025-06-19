import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const maxInt32 = Math.pow(2, 31) - 1;

// Restore sane parameters for the end of a test to ensure
// rate limiting is disabled during shutdown.
const kParamsRestore = {
    ingressRequestAdmissionRatePerSec: maxInt32,
    ingressRequestAdmissionBurstSize: maxInt32,
    ingressRequestRateLimiterEnabled: 0,
};

/**
 * Value for app name based exemption in the ingress request rate limiter.
 */
export const kRateLimiterExemptAppName = "testRateLimiter";

/**
 * Common configuration for rate limiting tests.
 * It enables the feature flags and enables fail points.
 */
export const kConfigLogsAndFailPointsForRateLimiterTests = {
    logComponentVerbosity: tojson({command: 2}),
    featureFlagIngressRateLimiting: 1,
    "failpoint.ingressRateLimiterVerySlowRate": tojson({
        mode: "alwaysOn",
    }),
    "failpoint.skipRateLimiterForTestClient":
        tojson({mode: "alwaysOn", data: {exemptAppName: kRateLimiterExemptAppName}}),
};

const kUser = "admin";
const kPass = "pwd";

/**
 * Setup authentication minimally. Creates the user and ensures that the exempt connection
 * has necessary authentication to set server parameters.
 *
 * To authenticate connections other than the exempt conn, use 'authenticateConnection'.
 */
function setupAuth(conn, exemptConn) {
    // Since rate limiting only applies when authenticated, create a user and authenticate.
    const admin = conn.getDB('admin');
    admin.createUser({user: kUser, pwd: kPass, roles: ['root']});
    exemptConn.getDB('admin').auth(kUser, kPass);
}

/**
 * Authenticate the connection using a predefined admin user.
 */
export function authenticateConnection(conn) {
    const admin = conn.getDB('admin');
    admin.auth(kUser, kPass);
}

/**
 * Returns the stats for the ingress request rate limiter.
 */
export function getRateLimiterStats(exemptConn) {
    const db = exemptConn.getDB("admin");
    const status = db.serverStatus();
    return status.network.ingressRequestRateLimiter;
}

/**
 * Runs a test for the ingress admission rate limiter using a single mongod process.
 */
export function runTestStandalone({startupParams, auth, cmdParams = {}}, testFunction) {
    const mongod = MongoRunner.runMongod({
        ...cmdParams,
        auth: auth ? '' : undefined,
        setParameter: {
            ...kConfigLogsAndFailPointsForRateLimiterTests,
            ...startupParams,
        },
    });

    const exemptConn = new Mongo(`mongodb://${mongod.host}/?appName=${kRateLimiterExemptAppName}`);

    if (auth) {
        setupAuth(mongod, exemptConn);
    }

    testFunction(mongod, exemptConn);
    assert.commandWorked(exemptConn.adminCommand({setParameter: 1, ...kParamsRestore}));

    MongoRunner.stopMongod(mongod);
}

/**
 * Runs a test for the ingress admission rate limiter using a replset.
 */
export function runTestReplSet({startupParams, auth, cmdParams = {}}, testFunction) {
    const replSet = new ReplSetTest({
        nodes: 1,
        keyFile: "jstests/libs/key1",
        nodeOptions: {
            ...cmdParams,
            auth: auth ? '' : undefined,
            setParameter: {
                ...kConfigLogsAndFailPointsForRateLimiterTests,
                ...startupParams,
                ingressRequestRateLimiterEnabled: 0,  // kept disabled during repl set setup
            },
        },
    });

    // We setup the replset safely with rate limiting disabled.
    replSet.startSet();
    replSet.initiate();

    const primary = replSet.getPrimary();
    const exemptConn = new Mongo(`mongodb://${primary.host}/?appName=${kRateLimiterExemptAppName}`);
    setupAuth(primary, exemptConn);
    const exemptAdmin = exemptConn.getDB("admin");

    const {ingressRequestRateLimiterEnabled} = startupParams;
    if ((ingressRequestRateLimiterEnabled ?? 0) !== 0) {
        assert.commandWorked(
            exemptAdmin.adminCommand({setParameter: 1, ingressRequestRateLimiterEnabled}));
    }

    testFunction(primary, exemptConn);

    assert.commandWorked(exemptAdmin.adminCommand({setParameter: 1, ...kParamsRestore}));
    replSet.stopSet();
}

/**
 * Runs a test for the ingress admission rate limiter using sharding.
 */
export function runTestSharded({startupParams, auth, cmdParamsMongos = {}, cmdParamsMongod = {}},
                               testFunction) {
    const st = new ShardingTest({
        mongos: 1,
        shards: 1,
        other: {
            auth: auth ? '' : undefined,
            keyFile: "jstests/libs/key1",
            mongosOptions: {
                ...cmdParamsMongos,
                setParameter: {
                    ...kConfigLogsAndFailPointsForRateLimiterTests,
                    ...startupParams,
                    ingressRequestRateLimiterEnabled: 0,  // kept disabled during sharding setup
                },
            },
            rsOptions: {
                ...cmdParamsMongod,
                setParameter: {
                    ...kConfigLogsAndFailPointsForRateLimiterTests,
                    ...startupParams,
                    ingressRequestRateLimiterEnabled: 0,  // kept disabled during sharding setup
                },
            },
        }
    });

    const exemptConn = new Mongo(`mongodb://${st.s.host}/?appName=${kRateLimiterExemptAppName}`);
    setupAuth(st.s, exemptConn);
    const exemptAdmin = exemptConn.getDB("admin");

    const {ingressRequestRateLimiterEnabled} = startupParams;
    if ((ingressRequestRateLimiterEnabled ?? 0) !== 0) {
        assert.commandWorked(
            exemptAdmin.adminCommand({setParameter: 1, ingressRequestRateLimiterEnabled}));
    }

    testFunction(st.s, exemptConn);

    assert.commandWorked(exemptAdmin.adminCommand({setParameter: 1, ...kParamsRestore}));
    st.stop();
}
