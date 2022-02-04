/**
 * This test checks that cluster IP allowlists can be set and respected.
 */

(function() {
'use strict';

print("When allowlist is empty, the server does not start.");
assert.throws(() => MongoRunner.runMongod(
                  {auth: null, keyFile: "jstests/libs/key1", clusterIpSourceAllowlist: ""}),
              [],
              "The server unexpectedly started");
// Check that the same behavior is seen with the deprecated 'clusterIpSourceWhiteList' flag.
assert.throws(() => MongoRunner.runMongod(
                  {auth: null, keyFile: "jstests/libs/key1", clusterIpSourceWhitelist: ""}),
              [],
              "The server unexpectedly started");

function emitWarningAuthErrorIsExpected(authResult) {
    if (!authResult) {
        print("***** NOTE: an authentication error is expected");
    }
}

function testIpAllowlistStartup(description, allowlistString, authResult) {
    print("Startup: " + description);

    let conn = MongoRunner.runMongod(
        {auth: null, keyFile: "jstests/libs/key1", clusterIpSourceAllowlist: allowlistString});
    assert.eq(authResult, conn.getDB("local").auth("__system", "foopdedoop"));
    emitWarningAuthErrorIsExpected(authResult);
    MongoRunner.stopMongod(conn);

    // Verify that the deprecated 'clusterIpSourceWhitelist' flag still exhibits the same behavior.
    conn = MongoRunner.runMongod(
        {auth: null, keyFile: "jstests/libs/key1", clusterIpSourceWhitelist: allowlistString});
    assert.eq(authResult, conn.getDB("local").auth("__system", "foopdedoop"));
    emitWarningAuthErrorIsExpected(authResult);
    MongoRunner.stopMongod(conn);
}

function testIpAllowlistRuntime(description, allowlistString, authResult) {
    print("Runtime: " + description);

    const conn = MongoRunner.runMongod({keyFile: "jstests/libs/key1"});
    const admin = conn.getDB('admin');
    const local = conn.getDB('local');

    // Must create a user to verify that we don't fallback to localhost exception for auth.
    assert.commandWorked(admin.runCommand({createUser: 'admin', pwd: 'admin', roles: ['root']}));

    assert(local.auth("__system", "foopdedoop"));

    print("Testing whether __system can login after set to: " + allowlistString);
    assert.commandWorked(admin.runCommand(
        {setParameter: 1, "clusterIpSourceAllowlist": allowlistString.split(",")}));
    if (!authResult) {
        // At this time we have no valid authentication, and existing session should reset.
        // We should expect admin commands to fail.
        print("Verifying that there is no longer authenticated session and admin commands fail");
        assert.commandFailed(conn.adminCommand({fsync: 1}));
    }

    let authDB = local;
    assert.eq(authResult, local.auth("__system", "foopdedoop"));
    emitWarningAuthErrorIsExpected(authResult);

    if (!authResult) {
        print("Authenticating with admin user since __system is barred");
        assert(admin.auth('admin', 'admin'));
        authDB = admin;
    }

    print("Testing that __system can login after reset to null");
    assert.commandWorked(admin.runCommand({setParameter: 1, "clusterIpSourceAllowlist": null}));
    authDB.logout();

    assert.eq(true, local.auth("__system", "foopdedoop"));
    MongoRunner.stopMongod(conn);
}

function testIpAllowlist(description, allowlistString, authResult) {
    testIpAllowlistStartup(description, allowlistString, authResult);
    testIpAllowlistRuntime(description, allowlistString, authResult);
}

function testIpAllowlistRuntimeGarbage() {
    print("Testing that garbage input is handled");

    const conn = MongoRunner.runMongod({auth: null, keyFile: "jstests/libs/key1"});
    assert.eq(true, conn.getDB("local").auth("__system", "foopdedoop"));

    const BAD_INPUTS = [
        [""],
        ["abcxyz"],
        ["1.1.1.1", "abcxyz"],
        ["1.1.1.1/abcxyz"],
        "1.1.1.1",
        1,
        {"something": "something else"},
        ["1.1.1.1", {"something": "something else"}],
    ];

    for (let bi in BAD_INPUTS) {
        print(bi);
        assert.commandFailed(conn.adminCommand({setParameter: 1, "clusterIpSourceAllowlist": bi}));
    }

    print("Testing that __system can login after reset to null");
    assert.commandWorked(conn.adminCommand({setParameter: 1, "clusterIpSourceAllowlist": null}));
    conn.getDB("local").logout();
    assert.eq(true, conn.getDB("local").auth("__system", "foopdedoop"));
    MongoRunner.stopMongod(conn);
}

testIpAllowlist(
    "When 127.0.0.1 is allowlisted, a client connected via localhost may auth as __system.",
    "127.0.0.1",
    true);

testIpAllowlist(
    "When 127.0.0.0 is allowlisted as a 24-bit CIDR block, a client connected via localhost may auth as __system.",
    "127.0.0.0/24",
    true);

testIpAllowlist(
    "When 127.0.0.5 is allowlisted as a 24-bit CIDR block, a client connected via localhost may auth as __system.",
    "127.0.0.5/24",
    true);

testIpAllowlist(
    "When 127.0.0.0 is allowlisted as a 8-bit CIDR block, a client connected via localhost may auth as __system.",
    "127.0.0.0/8",
    true);

testIpAllowlist(
    "When the IP block reserved for documentation and the 127.0.0.0/8 block are both allowlisted, a client connected via localhost may auth as __system.",
    "192.0.2.0/24,127.0.0.0/8",
    true);

testIpAllowlist(
    "When 127.0.0.0/8 and the IP block reserved for documentation are both allowlisted, a client connected via localhost may auth as __system.",
    "127.0.0.0/8,192.0.2.0/24",
    true);

testIpAllowlist(
    "When the IP block reserved for documentation and examples is allowlisted, a client connected via localhost may not auth as __system.",
    "192.0.2.0/24",
    false);

testIpAllowlistRuntimeGarbage();
}());
