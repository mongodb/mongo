/**
 * This test checks if different mixtures of ssl modes
 * in a sharded clutster can or cannot function
 */

load("jstests/ssl/libs/ssl_helpers.js");

// Due to mixed SSL mode settings, a shard will be unable to establish an outgoing
// connection to the config server in order to load relevant collection UUIDs into
// its config.cache.collections collection. The consistency check verifies the
// shard's config.cache.collections UUIDs, so it may fail.
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;

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
