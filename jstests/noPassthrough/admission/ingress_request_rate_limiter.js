
/**
 * Test that the ingress admission rate limiter works correctly.
 * @tags: [requires_fcv_80]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const maxInt32 = Math.pow(2, 31) - 1;

/**
 * Runs the set parameter commands with some arbitrary value to ensure invalid values are rejected
 */
function testServerParameter(conn) {
    const db = conn.getDB("admin");
    // Test setting burst size
    assert.commandFailedWithCode(db.adminCommand({
        setParameter: 1,
        ingressRequestAdmissionBurstSize: 0,
    }),
                                 ErrorCodes.BadValue);

    assert.commandFailedWithCode(db.adminCommand({
        setParameter: 1,
        ingressRequestAdmissionBurstSize: -1,
    }),
                                 ErrorCodes.BadValue);

    assert.commandWorked(db.adminCommand({setParameter: 1, ingressRequestAdmissionBurstSize: 1}));

    assert.commandWorked(db.adminCommand({setParameter: 1, ingressRequestAdmissionBurstSize: 2}));

    // Test setting admission rate

    assert.commandFailedWithCode(db.adminCommand({
        setParameter: 1,
        ingressRequestAdmissionRatePerSec: -1,
    }),
                                 ErrorCodes.BadValue);

    assert.commandWorked(db.adminCommand({
        setParameter: 1,
        ingressRequestAdmissionRatePerSec: 1,
    }));

    // Restore server parameters
    assert.commandWorked(db.adminCommand({
        setParameter: 1,
        ingressRequestAdmissionRatePerSec: maxInt32,
        ingressRequestAdmissionBurstSize: maxInt32
    }));
}

// Arbitrary but low burst size in the amount of commands allowed to pass through.
const amountOfInserts = 3;
const maxBurstSize = amountOfInserts;

// Since the rate limiter is set to a very low rate and a burst of 3,
// the 3 additional extra will be rejected and will result in an error.
const extraRequests = 3;

/**
 * This function compares the metrics before a test and after a test to ensure they are consistent
 *
 * This function was factored out since some tests other than testRateLimiterMetrics are doing
 * the same checks.
 */
function assertMetrics(
    initialStatus, finalStatus, expectedAmountOfSuccess, expectedAmountOfFailures) {
    const initialIngressRequestRateLimiter = initialStatus.network.ingressRequestRateLimiter;
    const initialInserts = initialStatus.metrics.commands.insert.total;

    const ingressRequestRateLimiter = finalStatus.network.ingressRequestRateLimiter;
    const inserts = finalStatus.metrics.commands.insert.total;

    assert.eq(ingressRequestRateLimiter.successfulAdmissions -
                  initialIngressRequestRateLimiter.successfulAdmissions,
              expectedAmountOfSuccess);
    assert.eq(ingressRequestRateLimiter.rejectedAdmissions -
                  initialIngressRequestRateLimiter.rejectedAdmissions,
              expectedAmountOfFailures);
    assert.eq(ingressRequestRateLimiter.attemptedAdmissions -
                  initialIngressRequestRateLimiter.attemptedAdmissions,
              expectedAmountOfSuccess + expectedAmountOfFailures);
    assert.eq(inserts - initialInserts, expectedAmountOfSuccess);

    // We also ensure all token have been consumed
    assert.eq(Math.floor(ingressRequestRateLimiter.totalAvailableTokens), 0);
}

/**
 * Runs a fixed amount of operations and verifies the difference in the metrics
 *
 * It also verifies that the errors has the right error label
 */
function testRateLimiterMetrics(conn, exemptConn) {
    const db = exemptConn.getDB(`${jsTest.name()}_db`);
    const initialStatus = db.serverStatus();

    // Here we calculate the amount of requests that are expected to be successful using the
    // available amount of token in the rate limiter
    const requestAmount = amountOfInserts + extraRequests;
    const expectedAmountOfSuccess =
        Math.floor(initialStatus.network.ingressRequestRateLimiter.totalAvailableTokens);
    const expectedAmountOfFailures = requestAmount - expectedAmountOfSuccess;

    // We want to ensure we do at least one successful request and one failed request
    assert.gt(expectedAmountOfSuccess, 0);
    assert.gt(expectedAmountOfFailures, 0);

    // We run inserts command that will either pass or fail. When it fails, we validate the error
    // code and label
    for (let i = 0; i < requestAmount; ++i) {
        const assertContainSystemOverloadedErrorLabel = (res) => {
            assert(res.hasOwnProperty("errorLabels"), res);
            assert.sameMembers(["SystemOverloadedError"], res.errorLabels);
        };

        const collName = `${jsTest.name()}_coll`;

        const db = conn.getDB(`${jsTest.name()}_db`);
        const result = db.runCommand({insert: collName, documents: [{dummy: 1}]});

        if (result.ok === 0) {
            assert.commandFailedWithCode(result, ErrorCodes.RateLimitExceeded);
            assertContainSystemOverloadedErrorLabel(result);
        }
    }

    const status = db.serverStatus();
    assertMetrics(initialStatus, status, expectedAmountOfSuccess, expectedAmountOfFailures);
}

/**
 * Runs some ping command with an unauthenticated client and validates that all requests were
 * exempt. This test only work on server with authentication enabled.
 */
function testRateLimiterUnauthenticated(noAuthClient, exemptConn) {
    // We run more pings than the available burst size. As all pings are exempts, they should
    // all succeed.
    const amountOfPing = maxBurstSize + extraRequests;
    const db = exemptConn.getDB(`${jsTest.name()}_db`);
    const initialStatus = db.serverStatus();
    const initialExempt = initialStatus.network.ingressRequestRateLimiter.exemptedAdmissions;
    const initialAvailableTokens =
        initialStatus.network.ingressRequestRateLimiter.totalAvailableTokens;

    // Run ping on unauthenticated client. As the client is unauthenticated, all command
    // should work as there is no rate limiting applied
    const noAuthDB = noAuthClient.getDB("admin");
    for (let i = 0; i < maxBurstSize + extraRequests; ++i) {
        assert.commandWorked(noAuthDB.runCommand({ping: 1}));
    }

    const finalStatus = db.serverStatus();
    const finalExempt = finalStatus.network.ingressRequestRateLimiter.exemptedAdmissions;
    const finalAvailableTokens = finalStatus.network.ingressRequestRateLimiter.totalAvailableTokens;

    // Check that we added as much exempt requests as the amount of ping we ran. As no requests
    // should be rate limited, running a command should only increment the exempt counter
    assert.eq(finalExempt - initialExempt, amountOfPing);

    // Check that we didn't affect the amount of available tokens while running pings
    assert.eq(finalAvailableTokens - initialAvailableTokens, 0);
}

function setupAuth(conn, exemptConn) {
    // Since rate limiting only applies when authenticated, create a user and authenticate
    const admin = conn.getDB('admin');
    admin.createUser({user: 'admin', pwd: 'pwd', roles: ['root']});
    admin.auth('admin', 'pwd');
    exemptConn.getDB('admin').auth('admin', 'pwd');
}

const kExemptAppName = "testRateLimiter";

// Parameters for ingress admission rate limiting enabled
const kParams = {
    ingressRequestAdmissionRatePerSec: 1,
    ingressRequestAdmissionBurstSize: maxBurstSize,
    ingressRequestRateLimiterEnabled: true,
};

// Parameters for ingress admission rate limiting disabled
const kParamsRestore = {
    ingressRequestAdmissionRatePerSec: maxInt32,
    ingressRequestAdmissionBurstSize: maxInt32,
    ingressRequestRateLimiterEnabled: false,
};

const kStartupConfig = {
    logComponentVerbosity: tojson({command: 2}),
    featureFlagIngressRateLimiting: 1,
    "failpoint.ingressRateLimiterVerySlowRate": tojson({
        mode: "alwaysOn",
    }),
    "failpoint.skipRateLimiterForTestClient":
        tojson({mode: "alwaysOn", data: {exemptAppName: "testRateLimiter"}}),
};

/**
 * This function tests the ingress admission rate limiter when it is enabled at startup
 */
function runTestParamStartup() {
    const mongod = MongoRunner.runMongod({
        auth: '',
        setParameter: {
            ...kParams,
            ...kStartupConfig,
        },
    });

    const exemptConn = new Mongo(`mongodb://${mongod.host}/?appName=${kExemptAppName}`);
    setupAuth(mongod, exemptConn);

    testRateLimiterMetrics(mongod, exemptConn);
    assert.commandWorked(exemptConn.adminCommand({setParameter: 1, ...kParamsRestore}));

    MongoRunner.stopMongod(mongod);
}

/**
 * This function tests the ingress admission rate limiter when it is enabled at runtime
 */
function runTestParamRuntime() {
    const mongod = MongoRunner.runMongod({auth: '', setParameter: kStartupConfig});
    const exemptConn = new Mongo(`mongodb://${mongod.host}/?appName=${kExemptAppName}`);
    const noAuthClient = new Mongo(mongod.host);

    setupAuth(mongod, exemptConn);
    assert.commandWorked(exemptConn.adminCommand({setParameter: 1, ...kParams}));

    // This tests will run ping commands on the unauthenticated client. As auth is enabled on the
    // server, no token should be consumed
    testRateLimiterUnauthenticated(noAuthClient, exemptConn);

    assert.commandWorked(exemptConn.adminCommand({setParameter: 1, ...kParams}));
    testRateLimiterMetrics(mongod, exemptConn);
    assert.commandWorked(exemptConn.adminCommand({setParameter: 1, ...kParamsRestore}));

    MongoRunner.stopMongod(mongod);
}

/**
 * This function tests the ingress admission rate limiter when authentication is disabled and
 * clients are unauthenticated
 */
function runTestAuthDisabled() {
    const mongod = MongoRunner.runMongod({setParameter: kStartupConfig});
    const exemptConn = new Mongo(`mongodb://${mongod.host}/?appName=${kExemptAppName}`);

    // Here we skip authentication setup
    // When auth is disabled, we expect rate limiting for both authenticated and unauthenticated
    // clients

    assert.commandWorked(exemptConn.adminCommand({setParameter: 1, ...kParams}));
    testRateLimiterMetrics(mongod, exemptConn);
    assert.commandWorked(exemptConn.adminCommand({setParameter: 1, ...kParamsRestore}));

    MongoRunner.stopMongod(mongod);
}

/**
 * Replica set test to check if rate limiting also applies to other topology
 */
function runTestReplSet() {
    const replSet = new ReplSetTest({
        nodes: 1,
        keyFile: "jstests/libs/key1",
        nodeOptions: {
            auth: '',
            setParameter: {
                ...kParams,
                ...kStartupConfig,
                ingressRequestRateLimiterEnabled: 0,  // kept disable during repl set setup
            },
        },
    });

    // We setup the replset safely with rate limiting disabled
    replSet.startSet();
    replSet.initiate();

    const primary = replSet.getPrimary();
    const exemptConn = new Mongo(`mongodb://${primary.host}/?appName=${kExemptAppName}`);

    setupAuth(primary, exemptConn);
    const exemptAdmin = exemptConn.getDB("admin");

    // Enable rate limiting now that the replset is up
    assert.commandWorked(
        exemptAdmin.adminCommand({setParameter: 1, ingressRequestRateLimiterEnabled: 1}));
    testRateLimiterMetrics(primary, exemptConn);

    assert.commandWorked(exemptAdmin.adminCommand({setParameter: 1, ...kParamsRestore}));
    replSet.stopSet();
}

/**
 * Sharding test to check if rate limiting also applies to sharded clusters
 */
function runTestSharded() {
    const st = new ShardingTest({
        mongos: 1,
        shards: 1,
        other: {
            auth: '',
            keyFile: "jstests/libs/key1",
            mongosOptions: {
                setParameter: {
                    ...kParams,
                    ...kStartupConfig,
                    ingressRequestRateLimiterEnabled: 0,  // kept disable during sharding setup
                },
            },
            rsOptions: {
                setParameter: {
                    ...kParams,
                    ...kStartupConfig,
                    ingressRequestRateLimiterEnabled: 0,  // kept disable during sharding setup
                },
            },
        }
    });

    // We create a connection with the exempted app name, used in the test to get server status
    const exemptConn = new Mongo(`mongodb://${st.s.host}/?appName=${kExemptAppName}`);

    setupAuth(st.s, exemptConn);
    const exemptAdmin = exemptConn.getDB("admin");

    // Enable rate limiting now that the replset is up
    assert.commandWorked(
        exemptAdmin.adminCommand({setParameter: 1, ingressRequestRateLimiterEnabled: 1}));
    testRateLimiterMetrics(st.s, exemptConn);

    assert.commandWorked(exemptAdmin.adminCommand({setParameter: 1, ...kParamsRestore}));
    st.stop();
}

/**
 * This function tests the ingress admission rate limiter when with a compressed client
 *
 * This test is different than the others because for compression to work a parallelShell is
 * needed.
 */
function runTestCompressed() {
    const mongod = MongoRunner.runMongod({
        networkMessageCompressors: "snappy",
        setParameter: {
            ...kParams,
            ...kStartupConfig,
            ingressRequestRateLimiterEnabled: false,
        },
    });

    const kCompressor = "snappy";

    // We create an exempt connection in order to get the status without affecting the metrics
    const compressedConn = new Mongo(`mongodb://${mongod.host}/?compressors=${kCompressor}`);
    const exemptConn = new Mongo(`mongodb://${mongod.host}/?appName=${kExemptAppName}`);
    const admin = exemptConn.getDB("admin");

    // We calculate the amount of expected success and failures depending on the amount of token
    // available
    const initialStatus = admin.serverStatus();
    const initialAvailableTokens =
        initialStatus.network.ingressRequestRateLimiter.totalAvailableTokens;

    const requestAmount = amountOfInserts + extraRequests;
    const expectedAmountOfSuccess =
        Math.floor(initialStatus.network.ingressRequestRateLimiter.totalAvailableTokens);
    const expectedAmountOfFailures = requestAmount - expectedAmountOfSuccess;

    // Here we run a parallel shell so we can enable network compression
    const compressionArgs = ["--networkMessageCompressors", kCompressor];
    const shell = startParallelShell(
        funWithArgs(
            (host, params, exemptAppName, requestAmount) => {
                const exemptConn = new Mongo(`mongodb://${host}/?appName=${exemptAppName}`);
                exemptConn.getDB("admin").auth("admin", "pwd");
                assert.commandWorked(
                    exemptConn.adminCommand({setParameter: 1, ...JSON.parse(params)}));

                // We run inserts command that will either pass or fail. When it fails, we validate
                // the error code and label
                for (let i = 0; i < requestAmount; ++i) {
                    const assertContainSystemOverloadedErrorLabel = (res) => {
                        assert(res.hasOwnProperty("errorLabels"), res);
                        assert.sameMembers(["SystemOverloadedError"], res.errorLabels);
                    };

                    const collName = `${jsTest.name()}_coll`;

                    const result = db.runCommand({insert: collName, documents: [{dummy: 1}]});

                    if (result.ok === 0) {
                        assert.commandFailedWithCode(result, ErrorCodes.RateLimitExceeded);
                        assertContainSystemOverloadedErrorLabel(result);
                    }
                }
                assert.commandWorked(exemptConn.adminCommand(
                    {setParameter: 1, ingressRequestRateLimiterEnabled: 0}));
            },
            mongod.host,
            JSON.stringify(kParams),
            kExemptAppName,
            requestAmount),
        mongod.port,
        false,
        ...compressionArgs);

    shell();

    // We get the final metrics so we can compare and assert on the difference
    const finalStatus = admin.serverStatus();
    const finalExempt = finalStatus.network.ingressRequestRateLimiter.exemptedAdmissions;
    const finalAvailableTokens = finalStatus.network.ingressRequestRateLimiter.totalAvailableTokens;

    assertMetrics(initialStatus, finalStatus, expectedAmountOfSuccess, expectedAmountOfFailures);

    MongoRunner.stopMongod(mongod);
}

runTestParamStartup();
runTestParamRuntime();
runTestAuthDisabled();
runTestReplSet();
runTestSharded();
runTestCompressed();
