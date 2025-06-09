
/**
 * Test that the ingress admission rate limiter works correctly.
 * @tags: [requires_fcv_80]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const maxInt32 = Math.pow(2, 31) - 1;

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
const maxBurstSize = 3;

const testRateLimiterError = (conn) => {
    // Start maxBurstSize + 3 threads that will all try admitting a request through the ingress
    // admission rate limiter. Since the rate limiter is set to a very low rate and a burst of 3,
    // the 3 additional extra will be rejected and will result in an error.
    const extraRequests = 3;

    const db = conn.getDB(`${jsTest.name()}_db`);
    const initialStatus = db.serverStatus();
    const initialIngressRequestRateLimiter = initialStatus.network.ingressRequestRateLimiter;
    const initialInserts = initialStatus.metrics.commands.insert.total;
    assert.eq(initialIngressRequestRateLimiter.totalAvailableTokens, maxBurstSize);

    const makeThread = () => new Thread((host) => {
        const assertContainSystemOverloadedErrorLabel = (res) => {
            assert(res.hasOwnProperty("errorLabels"), res);
            assert.sameMembers(["SystemOverloadedError"], res.errorLabels);
        };

        const conn = new Mongo(host);
        conn.getDB("admin").auth('admin', 'pwd');

        const db = conn.getDB(`${jsTest.name()}_db`);
        const collName = `${jsTest.name()}_coll`;

        const result = db.runCommand({insert: collName, documents: [{dummy: 1}]});

        if (result.ok === 0) {
            assert.commandFailedWithCode(result, ErrorCodes.RateLimitExceeded);
            assertContainSystemOverloadedErrorLabel(result);
        }
    }, conn.host);

    // Spawn maxBurstSize + extraRequests (6) threads and contains them in an array
    // We run in threads since we want all operation to attempt admission at roughly the same time
    const threads = Array.from({length: maxBurstSize + extraRequests}, makeThread);

    // Start all threads
    threads.forEach(t => t.start());
    // Join all background threads
    threads.forEach(t => t.join());

    const status = db.serverStatus();
    const ingressRequestRateLimiter = status.network.ingressRequestRateLimiter;
    const inserts = status.metrics.commands.insert.total;

    // We assert that addedToQueue is 0 since queuing is disabled
    assert.eq(ingressRequestRateLimiter.successfulAdmissions -
                  initialIngressRequestRateLimiter.successfulAdmissions,
              maxBurstSize);
    assert.eq(ingressRequestRateLimiter.rejectedAdmissions -
                  initialIngressRequestRateLimiter.rejectedAdmissions,
              extraRequests);
    assert.eq(ingressRequestRateLimiter.attemptedAdmissions -
                  initialIngressRequestRateLimiter.attemptedAdmissions,
              maxBurstSize + extraRequests);
    assert.eq(inserts - initialInserts, maxBurstSize);
    assert.eq(ingressRequestRateLimiter.totalAvailableTokens, 0);
};

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

/**
 * This function tests the ingress admission rate limiter when it is enabled at startup
 */
function runTestParamStartup() {
    const mongod = MongoRunner.runMongod({
        setParameter: {
            ...kParams,
            logComponentVerbosity: tojson({command: 2}),
            featureFlagIngressRateLimiting: 1,
            "failpoint.ingressRateLimiterVerySlowRate": tojson({
                mode: "alwaysOn",
            }),
        },
    });

    // Since rate limiting only applies when authenticated, create a user to be used in
    // testRateLimiterError
    const admin = mongod.getDB('admin');
    admin.createUser({user: 'admin', pwd: 'pwd', roles: ['root']});
    admin.auth('admin', 'pwd');

    testRateLimiterError(mongod);

    assert.commandWorked(mongod.adminCommand({setParameter: 1, ...kParamsRestore}));
    MongoRunner.stopMongod(mongod);
}

/**
 * This function tests the ingress admission rate limiter when it is enabled at runtime
 */
function runTestParamRuntime() {
    const mongod = MongoRunner.runMongod({
        setParameter: {
            logComponentVerbosity: tojson({command: 2}),
            featureFlagIngressRateLimiting: 1,
            "failpoint.ingressRateLimiterVerySlowRate": tojson({
                mode: "alwaysOn",
            }),
        }
    });

    // Since rate limiting only applies when authenticated, create a user to be used in
    // testRateLimiterError
    const admin = mongod.getDB('admin');
    admin.createUser({user: 'admin', pwd: 'pwd', roles: ['root']});
    admin.auth('admin', 'pwd');

    assert.commandWorked(mongod.adminCommand({setParameter: 1, ...kParams}));
    testRateLimiterError(mongod);

    assert.commandWorked(mongod.adminCommand({setParameter: 1, ...kParamsRestore}));
    MongoRunner.stopMongod(mongod);
}

// TODO: SERVER-105814 Add auth enabled tests to verified that unauthenticated operations are
// exempt from rate limiting

/**
 * Replica set test to check if rate limiting also applies to other topology
 */
function runTestReplSet() {
    const replSet = new ReplSetTest({
        nodes: 1,
        nodeOptions: {
            setParameter: {
                ...kParams,
                logComponentVerbosity: tojson({command: 2}),
                featureFlagIngressRateLimiting: 1,
                ingressRequestRateLimiterEnabled: 0,  // kept disable during repl set setup
                "failpoint.ingressRateLimiterVerySlowRate": tojson({
                    mode: "alwaysOn",
                }),
            },
        },
    });

    // We setup the replset safely with rate limiting disabled
    replSet.startSet();
    replSet.initiate();

    const primary = replSet.getPrimary();

    // Since rate limiting only applies when authenticated, create a user to be used in
    // testRateLimiterError
    const admin = primary.getDB('admin');
    admin.createUser({user: 'admin', pwd: 'pwd', roles: ['root']});
    admin.auth('admin', 'pwd');

    // Enable rate limiting now that the replset is up
    assert.commandWorked(
        primary.adminCommand({setParameter: 1, ingressRequestRateLimiterEnabled: 1}));
    testRateLimiterError(primary);

    assert.commandWorked(primary.adminCommand({setParameter: 1, ...kParamsRestore}));
    replSet.stopSet();
}

runTestParamStartup();
runTestParamRuntime();
runTestReplSet();
