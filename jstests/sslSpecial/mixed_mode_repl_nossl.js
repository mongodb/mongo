/**
 * This test verifies that replica sets of different
 * mixed modes can still function
 */

load("jstests/ssl/libs/ssl_helpers.js");

// Limit the amount of time we'll wait on a failure.
// Apply equally to success tests as well so that
// a failure to complete replication is more likely to
// give us a false negative, than a false positive.
ReplSetTest.kDefaultTimeoutMS = 3 * 60 * 1000;

// Verify that disabled allows non-ssl connections
print("=== Testing disabled cluster ===");
replShouldSucceed("disabled-disabled", disabled, disabled);

// Test mixed sslMode allowSSL/preferSSL with non-ssl client
print("=== Testing allowSSL/preferSSL cluster ===");
replShouldSucceed("allow-prefer", allowSSL, preferSSL);

// Test mixed sslMode allowSSL/disabled with non-ssl client
print("=== Testing allowSSL/disabled cluster ===");
replShouldSucceed("allow-disabled", allowSSL, disabled);

// Test mixed sslMode disables/preferSSL - should fail with non-ssl client
print("=== Testing disabled/preferSSL cluster - SHOULD FAIL ===");
replShouldFail("disabled-disabled", disabled, preferSSL);
