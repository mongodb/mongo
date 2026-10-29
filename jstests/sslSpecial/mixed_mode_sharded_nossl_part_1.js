/**
 * This test checks if different mixtures of ssl modes
 * in a sharded clutster can or cannot function. This test is split in 2 parts since it was hitting
 * timeouts in slow variants.
 */

import {disabled, mixedShardTest, preferTLS} from "jstests/ssl/libs/ssl_helpers.js";

print("=== Testing disabled cluster ===");
mixedShardTest(disabled, disabled, true);

print("=== Testing disabled/preferTLS cluster - SHOULD FAIL ===");
mixedShardTest(disabled, preferTLS, false);
mixedShardTest(preferTLS, disabled, false);
