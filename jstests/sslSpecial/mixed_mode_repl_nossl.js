/**
 * This test verifies that replica sets of different
 * mixed modes can still function
 */

load("jstests/ssl/libs/ssl_helpers.js");

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
