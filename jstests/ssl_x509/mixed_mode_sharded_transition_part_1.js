/*
 * Tests simultaneous upgrade from noauth/no-ssl to x509/requireTLS on a sharded cluster.
 * The purpose is to verify the connectivity between mongos, config server, and the shards
 *
 * NOTE: This test is similar to mixed_mode_sharded_transition_nossl.js in the sslSpecial
 * test suite. This suite must use ssl so it cannot test modes without ssl.
 *
 * This test is split in 2 parts since it was hitting timeouts in slow variants.
 */

import {allowTLS, mixedShardTest, preferTLS} from "jstests/ssl/libs/ssl_helpers.js";

// These hooks need to be able to connect to the individual shards.
TestData.skipCheckOrphans = true;
TestData.skipCheckShardFilteringMetadata = true;

const transitionToX509allowTLS =
    Object.merge(allowTLS, {transitionToAuth: '', clusterAuthMode: 'x509'});
const transitionToX509preferTLS =
    Object.merge(preferTLS, {transitionToAuth: '', clusterAuthMode: 'x509'});

jsTest.log('=== Testing transitionToAuth/allowTLS - transitionToAuth/preferTLS cluster ===');
mixedShardTest(transitionToX509allowTLS, transitionToX509preferTLS, true);
mixedShardTest(transitionToX509preferTLS, transitionToX509allowTLS, true);

jsTest.log('=== Testing transitionToAuth/preferTLS - transitionToAuth/preferTLS cluster ===');
mixedShardTest(transitionToX509preferTLS, transitionToX509preferTLS, true);
