/**
 * This test verifies that replica sets of different
 * mixed modes can still function
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {
    allowTLS,
    disabled,
    preferTLS,
    replShouldFail,
    replShouldSucceed
} from "jstests/ssl/libs/ssl_helpers.js";

// Limit the amount of time we'll wait on a failure.
// Apply equally to success tests as well so that
// a failure to complete replication is more likely to
// give us a false negative, than a false positive.
ReplSetTest.kDefaultTimeoutMS = 3 * 60 * 1000;

// Verify that disabled allows non-ssl connections
print("=== Testing disabled cluster ===");
replShouldSucceed("disabled-disabled", disabled, disabled);

// Test mixed tlsMode allowTLS/preferTLS with non-ssl client
print("=== Testing allowTLS/preferTLS cluster ===");
replShouldSucceed("allow-prefer", allowTLS, preferTLS);

// Test mixed tlsMode allowTLS/disabled with non-ssl client
print("=== Testing allowTLS/disabled cluster ===");
replShouldSucceed("allow-disabled", allowTLS, disabled);

// Test mixed tlsMode disables/preferTLS - should fail with non-ssl client
print("=== Testing disabled/preferTLS cluster - SHOULD FAIL ===");
replShouldFail("disabled-disabled", disabled, preferTLS);
