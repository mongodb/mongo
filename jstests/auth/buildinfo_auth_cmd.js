/**
 * Tests behavior of buildInfo command with and without being authenticated.
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";

function assertResult(expect, result, context) {
    try {
        switch (expect) {
            case 'success':
                assert.commandWorked(result);
                assert(result.maxBsonObjectSize !== undefined, "Missing expected field(s)");
                break;
            case 'fail':
                assert.commandFailedWithCode(result, ErrorCodes.Unauthorized);
                break;
            case 'version':
                assert.commandWorked(result);
                // Strip non-reply fields:
                delete result['ok'];
                delete result['$clusterTime'];
                delete result['operationTime'];

                assert.eq(Object.keys(result).length, 2, "Too many fields in buildInfo reply");
                assert(result.version !== undefined, "Missing 'version' field");
                assert(result.versionArray !== undefined, "Missing 'versionArray' field");
                break;
            default:
                assert(false, `Unknown expect value: $expect`);
        }
    } catch (e) {
        jsTest.log({context: context, expect: expect, result: result});
        throw e;
    }
}

function runTestcase(adminConn, test, authEnabled) {
    if (test.mode !== null) {
        assert.commandWorked(
            adminConn.adminCommand({setParameter: 1, buildInfoAuthMode: test.mode}));
    }

    const conn = new Mongo(adminConn.host);
    assertResult(test.expect, conn.adminCommand({buildInfo: 1}), `${test.mode} pre-auth`);
    if (authEnabled) {
        assert(conn.getDB('admin').auth('user1', 'pwd'));
        assertResult('success', conn.adminCommand({buildInfo: 1}), `${test.mode} authenticated`);
    }
}

function runTest(conn, authEnabled) {
    const admin = conn.getDB('admin');

    jsTest.log('Running tests: ' + tojson(authEnabled));
    if (authEnabled) {
        admin.createUser({user: 'admin', pwd: 'pwd', roles: ['root']});
        assert(admin.auth('admin', 'pwd'));
        admin.createUser({user: 'user1', pwd: 'pwd', roles: []});
        jsTest.log('Created Users');
    }

    function expect(ex) {
        // When auth is not enabled (for whatever reason), we should always succeed.
        return authEnabled ? ex : 'success';
    }

    // Initial test case using the default setting should fail when unauthenticated.
    runTestcase(conn, {mode: null, expect: expect('fail')}, authEnabled);

    const testCases = [
        {mode: 'requiresAuth', expect: expect('fail')},
        {mode: 'versionOnlyIfPreAuth', expect: expect('version')},
        {mode: 'allowedPreAuth', expect: expect('success')},
    ];

    testCases.forEach((test) => runTestcase(conn, test, authEnabled));
}

[true,
 false]
    .forEach(function(authEnabled) {
        {
            // Test standalone.
            const opts = {useHostname: false};
            if (authEnabled) {
                opts.auth = '';
            }
            const m = MongoRunner.runMongod(opts);
            // localhostAuthBypass should allow buildInfo to work with auth enabled,
            // even though we're not yet authenticated.
            runTestcase(m, {mode: null, expect: 'success'}, false);

            // Now (order is important), create users and run the tests
            // expecting auth-enabled mode failure.
            runTest(m, authEnabled);

            MongoRunner.stopMongod(m);
        }

        {
            // Test sharded.
            const opts = {shards: 1, mongos: 1, config: 1};
            if (authEnabled) {
                opts.other = {keyFile: 'jstests/libs/key1'};
            }
            const st = new ShardingTest(opts);
            if (authEnabled) {
                // localhostAuthBypass only applies to shards, so we should fail pre-auth.
                runTestcase(st.s0, {mode: null, expect: 'fail'}, false);
            }
            runTest(st.s0, authEnabled);
            st.stop();
        }
    });
