/**
 * This test checks if different mixtures of ssl modes
 * in a sharded cluster can or cannot function
 */

// If we are running in use-x509 passthrough mode, turn it off
// since it is not necessary for this test.
TestData.useX509 = false;
load("jstests/ssl/libs/ssl_helpers.js");

print("=== Testing requireSSL/requireSSL cluster ===");
mixedShardTest(requireSSL, requireSSL, true);

print("=== Testing preferSSL/requireSSL cluster ===")
mixedShardTest(preferSSL, requireSSL, true);
mixedShardTest(requireSSL, preferSSL, true);

print("=== Testing allowSSL/preferSSL cluster ===");
mixedShardTest(preferSSL, allowSSL, true);
mixedShardTest(allowSSL, preferSSL, true);

print("=== Testing allowSSL/requireSSL cluster - SHOULD FAIL ===");
mixedShardTest(requireSSL, allowSSL,  false);
