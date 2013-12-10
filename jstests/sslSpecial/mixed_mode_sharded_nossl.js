/**
 * This test checks if different mixtures of ssl modes
 * in a sharded clutster can or cannot function
 */

load("jstests/ssl/libs/ssl_helpers.js");

print("=== Testing disabled cluster ===");
mixedShardTest(disabled, disabled, true);

print("=== Testing disabled/preferSSL cluster - SHOULD FAIL ===");
mixedShardTest(disabled, preferSSL, false);

print("=== Testing allowSSL/disabled cluster ===");
mixedShardTest(disabled, allowSSL, true);
mixedShardTest(allowSSL, disabled, true);

print("=== Testing allowSSL/preferSSL cluster ===");
mixedShardTest(preferSSL, allowSSL, true);
mixedShardTest(allowSSL, preferSSL, true);

print("=== Testing allowSSL/requireSSL cluster - SHOULD FAIL ===");
mixedShardTest(allowSSL, requireSSL, false);

print("=== Testing disabled/requireSSL cluster - SHOULD FAIL ===");
mixedShardTest(disabled, requireSSL, false);
