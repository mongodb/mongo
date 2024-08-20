// This test is related to mixed_mode_repl_nossl.js in
// the sslSpecial test set. This test must be run with --use-ssl

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {
    allowTLS,
    preferTLS,
    replShouldFail,
    replShouldSucceed,
    requireTLS
} from "jstests/ssl/libs/ssl_helpers.js";

// Limit the amount of time we'll wait on a failure.
// Apply equally to success tests as well so that
// a failure to complete replication is more likely to
// give us a false negative, than a false positive.
ReplSetTest.kDefaultTimeoutMS = 3 * 60 * 1000;

// Verify that requireTLS allows ssl connections
print("=== Testing requireTLS/requireTLS cluster ===");
replShouldSucceed("require-require", requireTLS, requireTLS);

// Test mixed tlsMode allowTLS/preferTLS
print("=== Testing allowTLS/preferTLS cluster ===");
replShouldSucceed("allow-prefer", allowTLS, preferTLS);

// Test mixed tlsMode preferTLS/requireTLS
print("=== Testing preferTLS/requireTLS cluster ===");
replShouldSucceed("prefer-require", preferTLS, requireTLS);

// Test mixed tlsMode disabled/preferTLS - should fail
print("=== Testing allowTLS/requireTLS cluster - SHOULD FAIL ===");
replShouldFail("allow-require", allowTLS, requireTLS);
