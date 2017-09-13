/**
 * This test checks that cluster IP whitelists can be set and respected.
 */

(function() {
    'use strict';

    print("When whitelist is empty, the server does not start.");
    assert.eq(null,
              MongoRunner.runMongod(
                  {auth: null, keyFile: "jstests/libs/key1", clusterIpSourceWhitelist: ""}));

    function testIpWhitelist(description, whitelistString, authResult) {
        print(description);

        var conn = MongoRunner.runMongod(
            {auth: null, keyFile: "jstests/libs/key1", clusterIpSourceWhitelist: whitelistString});
        assert.eq(authResult, conn.getDB("local").auth("__system", "foopdedoop"));
        MongoRunner.stopMongod(conn);
    }

    testIpWhitelist(
        "When 127.0.0.1 is whitelisted, a client connected via localhost may auth as __system.",
        "127.0.0.1",
        true);

    testIpWhitelist(
        "When 127.0.0.0 is whitelisted as a 24-bit CIDR block, a client connected via localhost may auth as __system.",
        "127.0.0.0/24",
        true);

    testIpWhitelist(
        "When 127.0.0.5 is whitelisted as a 24-bit CIDR block, a client connected via localhost may auth as __system.",
        "127.0.0.5/24",
        true);

    testIpWhitelist(
        "When 127.0.0.0 is whitelisted as a 8-bit CIDR block, a client connected via localhost may auth as __system.",
        "127.0.0.0/8",
        true);

    testIpWhitelist(
        "When the IP block reserved for documentation and the 127.0.0.0/8 block are both whitelisted, a client connected via localhost may auth as __system.",
        "192.0.2.0/24,127.0.0.0/8",
        true);

    testIpWhitelist(
        "When 127.0.0.0/8 and the IP block reserved for documentation are both whitelisted, a client connected via localhost may auth as __system.",
        "127.0.0.0/8,192.0.2.0/24",
        true);

    testIpWhitelist(
        "When the IP block reserved for documentation and examples is whitelisted, a client connected via localhost may not auth as __system.",
        "192.0.2.0/24",
        false);

}());
