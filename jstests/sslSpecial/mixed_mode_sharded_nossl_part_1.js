/**
 * This test checks if different mixtures of ssl modes
 * in a sharded clutster can or cannot function. This test is split in 2 parts since it was hitting
 * timeouts in slow variants.
 */

import {disabled, mixedShardTest, preferTLS} from "jstests/ssl/libs/ssl_helpers.js";

// Due to mixed SSL mode settings, a shard will be unable to establish an outgoing
// connection to the config server in order to load relevant collection UUIDs into
// its config.cache.collections collection. The consistency check verifies the
// shard's config.cache.collections UUIDs, so it may fail.
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;

print("=== Testing disabled cluster ===");
mixedShardTest(disabled, disabled, true);

print("=== Testing disabled/preferTLS cluster - SHOULD FAIL ===");
mixedShardTest(disabled, preferTLS, false);
mixedShardTest(preferTLS, disabled, false);
