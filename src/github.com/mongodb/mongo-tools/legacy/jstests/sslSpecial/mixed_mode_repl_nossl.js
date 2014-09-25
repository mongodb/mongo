/**
 * This test verifies that replica sets of different
 * mixed modes can still function
 */

load("jstests/ssl/libs/ssl_helpers.js")

// Verify that disabled allows non-ssl connections
print("=== Testing disabled cluster ===");
replShouldSucceed(disabled, disabled);

// Test mixed sslMode allowSSL/preferSSL with non-ssl client
print("=== Testing allowSSL/preferSSL cluster ===");
replShouldSucceed(allowSSL, preferSSL);

// Test mixed sslMode allowSSL/disabled with non-ssl client
print("=== Testing allowSSL/disabled cluster ===");
replShouldSucceed(allowSSL, disabled);

// Test mixed sslMode disables/preferSSL - should fail with non-ssl client
print("=== Testing disabled/preferSSL cluster - SHOULD FAIL ===");
replShouldFail(disabled, preferSSL);
