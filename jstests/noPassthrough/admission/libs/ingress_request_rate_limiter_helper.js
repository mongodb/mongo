import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const maxInt32 = Math.pow(2, 31) - 1;

// Restore sane parameters for the end of a test to ensure
// rate limiting is disabled during shutdown.
const kParamsRestore = {
    ingressRequestAdmissionRatePerSec: maxInt32,
    ingressRequestAdmissionBurstCapacitySecs: Number.MAX_VALUE,
    ingressRequestRateLimiterEnabled: 0,
};

/**
 * An abnormally slow refresh rate to assure that a refresh doesn't trigger during a particular
 * test.
 */
export const kSlowestRefreshRateSecs = 5e-6;

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
    "failpoint.ingressRequestRateLimiterFractionalRateOverride": tojson({
        mode: "alwaysOn",
        data: {rate: kSlowestRefreshRateSecs},
    }),
    ingressRequestRateLimiterApplicationExemptions: {appNames: [kRateLimiterExemptAppName]},
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
    const admin = conn.getDB("admin");
    admin.createUser({user: kUser, pwd: kPass, roles: ["root"]});
    exemptConn.getDB("admin").auth(kUser, kPass);
}

/**
 * Authenticate the connection using a predefined admin user.
 */
export function authenticateConnection(conn) {
    const admin = conn.getDB("admin");
    admin.auth(kUser, kPass);
}

/**
 * Returns a new authenticated non-exempt connection to host.
 */
export function makeAuthConn(host) {
    const conn = new Mongo(host);
    authenticateConnection(conn);
    return conn;
}

/**
 * Returns a new authenticated exempt connection to host.
 */
export function makeExemptConn(host) {
    const conn = new Mongo(`mongodb://${host}/?appName=${kRateLimiterExemptAppName}`);
    authenticateConnection(conn);
    return conn;
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
 * Measures ingress queue stats around an operation and returns the stat deltas.
 */
export function measureQueueStats(exemptConn, operationFn) {
    const before = getRateLimiterStats(exemptConn);
    operationFn();
    const after = getRateLimiterStats(exemptConn);

    return {
        addedToQueue: after.addedToQueue - before.addedToQueue,
        removedFromQueue: after.removedFromQueue - before.removedFromQueue,
        interruptedInQueue: after.interruptedInQueue - before.interruptedInQueue,
        rejectedAdmissions: after.rejectedAdmissions - before.rejectedAdmissions,
    };
}

/**
 * Runs the supplied function with the ingress request rate limiter disabled, restoring it
 * afterwards.
 */
export function withRateLimitingDisabled(exemptConn, fn) {
    assert.commandWorked(exemptConn.adminCommand({setParameter: 1, ingressRequestRateLimiterEnabled: 0}));
    try {
        fn();
    } finally {
        assert.commandWorked(exemptConn.adminCommand({setParameter: 1, ingressRequestRateLimiterEnabled: 1}));
    }
}

/**
 * Expected error labels present in command responses when requests are rejected by the
 * ingress request rate limiter.
 */
export const kExpectedErrorLabels = ["SystemOverloadedError", "RetryableError", "NoWritesPerformed"];

/**
 * Returns true if the expected rate limiting error labels are encountered in the command response.
 */
export function assertContainsExpectedErrorLabels(res) {
    assert(res.hasOwnProperty("errorLabels"), res);
    assert.sameMembers(kExpectedErrorLabels, res.errorLabels);
}

/**
 * Runs a test for the ingress admission rate limiter using a single mongod process.
 */
export function runTestStandalone({startupParams, auth, cmdParams = {}}, testFunction) {
    const mongod = MongoRunner.runMongod({
        ...cmdParams,
        auth: auth ? "" : undefined,
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
            auth: auth ? "" : undefined,
            setParameter: {
                ...kConfigLogsAndFailPointsForRateLimiterTests,
                ...startupParams,
                ingressRequestRateLimiterEnabled: 0, // kept disabled during repl set setup
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
        assert.commandWorked(exemptAdmin.adminCommand({setParameter: 1, ingressRequestRateLimiterEnabled}));
    }

    testFunction(primary, exemptConn);

    assert.commandWorked(exemptAdmin.adminCommand({setParameter: 1, ...kParamsRestore}));
    replSet.stopSet();
}

/**
 * Runs a test for the ingress admission rate limiter using sharding.
 */
export function runTestSharded({startupParams, auth, cmdParamsMongos = {}, cmdParamsMongod = {}}, testFunction) {
    const st = new ShardingTest({
        mongos: 1,
        shards: 1,
        other: {
            auth: auth ? "" : undefined,
            keyFile: "jstests/libs/key1",
            mongosOptions: {
                ...cmdParamsMongos,
                setParameter: {
                    ...kConfigLogsAndFailPointsForRateLimiterTests,
                    ...startupParams,
                    ingressRequestRateLimiterEnabled: 0, // kept disabled during sharding setup
                },
            },
            rsOptions: {
                ...cmdParamsMongod,
                setParameter: {
                    ...kConfigLogsAndFailPointsForRateLimiterTests,
                    ...startupParams,
                    ingressRequestRateLimiterEnabled: 0, // kept disabled during sharding setup
                },
            },
        },
    });

    const exemptConn = new Mongo(`mongodb://${st.s.host}/?appName=${kRateLimiterExemptAppName}`);
    setupAuth(st.s, exemptConn);
    const exemptAdmin = exemptConn.getDB("admin");

    const {ingressRequestRateLimiterEnabled} = startupParams;
    if ((ingressRequestRateLimiterEnabled ?? 0) !== 0) {
        assert.commandWorked(exemptAdmin.adminCommand({setParameter: 1, ingressRequestRateLimiterEnabled}));
    }

    testFunction(st.s, exemptConn);

    assert.commandWorked(exemptAdmin.adminCommand({setParameter: 1, ...kParamsRestore}));
    st.stop();
}
