// This test is related to mixed_mode_repl_nossl.js in
// the sslSpecial test set. This test must be run with --use-ssl

// If we are running in use-x509 passthrough mode, turn it off
// since it is not necessary for this test.
TestData.useX509 = false;
load("jstests/ssl/libs/ssl_helpers.js")

// Verify that requireSSL allows ssl connections
print("=== Testing requireSSL/requireSSL cluster ===");
replShouldSucceed( requireSSL, requireSSL);

// Test mixed sslMode allowSSL/preferSSL
print("=== Testing allowSSL/preferSSL cluster ===");
replShouldSucceed(allowSSL, preferSSL);

// Test mixed sslMode preferSSL/requireSSL
print("=== Testing preferSSL/requireSSL cluster ===")
replShouldSucceed( preferSSL, requireSSL);

// Test mixed sslMode disabled/preferSSL - should fail
print("=== Testing allowSSL/requireSSL cluster - SHOULD FAIL ===");
replShouldFail(allowSSL, requireSSL);
