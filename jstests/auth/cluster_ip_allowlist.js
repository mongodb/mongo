/**
 * This test checks that cluster IP allowlists can be set and respected.
 */

(function() {
'use strict';

print("When allowlist is empty, the server does not start.");
assert.eq(null,
          MongoRunner.runMongod(
              {auth: null, keyFile: "jstests/libs/key1", clusterIpSourceAllowlist: ""}));
// Check that the same behavior is seen with the deprecated 'clusterIpSourceWhiteList' flag.
assert.eq(null,
          MongoRunner.runMongod(
              {auth: null, keyFile: "jstests/libs/key1", clusterIpSourceWhitelist: ""}));

function testIpAllowlist(description, allowlistString, authResult) {
    print(description);

    var conn = MongoRunner.runMongod(
        {auth: null, keyFile: "jstests/libs/key1", clusterIpSourceAllowlist: allowlistString});
    assert.eq(authResult, conn.getDB("local").auth("__system", "foopdedoop"));
    MongoRunner.stopMongod(conn);

    // Verify that the deprecated 'clusterIpSourceWhitelist' flag still exhibits the same behavior.
    conn = MongoRunner.runMongod(
        {auth: null, keyFile: "jstests/libs/key1", clusterIpSourceWhitelist: allowlistString});
    assert.eq(authResult, conn.getDB("local").auth("__system", "foopdedoop"));
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
}());
