/**
 * This test checks if different mixtures of ssl modes
 * in a sharded cluster can or cannot function
 */
import {allowTLS, mixedShardTest, preferTLS, requireTLS} from "jstests/ssl/libs/ssl_helpers.js";

// Due to mixed SSL mode settings, a shard will be unable to establish an outgoing
// connection to the config server in order to load relevant collection UUIDs into
// its config.cache.collections collection. The consistency check verifies the
// shard's config.cache.collections UUIDs, so it may fail.
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;

print("=== Testing requireTLS/requireTLS cluster ===");
mixedShardTest(requireTLS, requireTLS, true);

print("=== Testing preferTLS/requireTLS cluster ===");
mixedShardTest(preferTLS, requireTLS, true);
mixedShardTest(requireTLS, preferTLS, true);

print("=== Testing allowTLS/preferTLS cluster ===");
mixedShardTest(preferTLS, allowTLS, true);
mixedShardTest(allowTLS, preferTLS, true);

print("=== Testing allowTLS/requireTLS cluster - SHOULD FAIL ===");
mixedShardTest(allowTLS, requireTLS, false);
