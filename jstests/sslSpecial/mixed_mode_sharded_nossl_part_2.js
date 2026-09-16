/**
 * This test checks if different mixtures of ssl modes
 * in a sharded clutster can or cannot function. This test is split in 2 parts since it was hitting
 * timeouts in slow variants.
 */

import {allowTLS, disabled, mixedShardTest, preferTLS} from "jstests/ssl/libs/ssl_helpers.js";

print("=== Testing allowTLS/disabled cluster ===");
mixedShardTest(disabled, allowTLS, true);
mixedShardTest(allowTLS, disabled, true);

print("=== Testing allowTLS/preferTLS cluster ===");
mixedShardTest(preferTLS, allowTLS, true);
mixedShardTest(allowTLS, preferTLS, true);
