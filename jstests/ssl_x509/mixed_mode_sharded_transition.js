/*
 * Tests simultaneous upgrade from noauth/no-ssl to x509/requireTLS on a sharded cluster.
 * The purpose is to verify the connectivity between mongos, config server, and the shards
 *
 * NOTE: This test is similar to the mixed_mode_sharded_transition.js in the sslSpecial
 * test suite. This suite must use ssl so it cannot test modes without ssl.
 */

import {allowTLS, mixedShardTest, preferTLS, requireTLS} from "jstests/ssl/libs/ssl_helpers.js";

// These hooks need to be able to connect to the individual shards.
TestData.skipCheckOrphans = true;
TestData.skipCheckShardFilteringMetadata = true;

var transitionToX509allowTLS =
    Object.merge(allowTLS, {transitionToAuth: '', clusterAuthMode: 'x509'});
var transitionToX509preferTLS =
    Object.merge(preferTLS, {transitionToAuth: '', clusterAuthMode: 'x509'});
var x509requireTLS = Object.merge(requireTLS, {clusterAuthMode: 'x509'});

function testCombos(opt1, opt2, shouldSucceed) {
    mixedShardTest(opt1, opt2, shouldSucceed);
    mixedShardTest(opt2, opt1, shouldSucceed);
}

print('=== Testing transitionToAuth/allowTLS - transitionToAuth/preferTLS cluster ===');
testCombos(transitionToX509allowTLS, transitionToX509preferTLS, true);

print('=== Testing transitionToAuth/preferTLS - transitionToAuth/preferTLS cluster ===');
mixedShardTest(transitionToX509preferTLS, transitionToX509preferTLS, true);

print('=== Testing transitionToAuth/preferTLS - x509/requireTLS cluster ===');
testCombos(transitionToX509preferTLS, x509requireTLS, true);
