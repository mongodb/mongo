/**
 * This test checks if different mixtures of ssl modes
 * in a sharded cluster can or cannot function
 */
import {allowTLS, mixedShardTest, preferTLS, requireTLS} from "jstests/ssl/libs/ssl_helpers.js";

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
mixedShardTest(requireTLS, allowTLS, false);
