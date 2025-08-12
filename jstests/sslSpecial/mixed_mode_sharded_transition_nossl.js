/*
 * Tests simultaneous upgrade from noauth/no-ssl to x509/requireTLS on a sharded cluster.
 * The purpose is to verify the connectivity between mongos, config server, and the shards
 *
 * NOTE: This test is similar to mixed_mode_sharded_transition_part_1/2.js in the ssl_x509
 * test suite. This suite does not use ssl so it cannot test modes with ssl.
 */

// Test setup randomly have auth/no auth setting on shards, which make hooks targetting shard
// directly more complicated. Skip the hooks since this test doesn't really do migrations.
TestData.skipCheckingIndexesConsistentAcrossCluster = true;
TestData.skipCheckOrphans = true;
TestData.skipCheckShardFilteringMetadata = true;

import {allowTLS, mixedShardTest} from "jstests/ssl/libs/ssl_helpers.js";

// Disable auth explicitly
var noAuthOptions = {noauth: ''};
var transitionToX509allowTLS =
    Object.merge(allowTLS, {transitionToAuth: '', clusterAuthMode: 'x509'});

print('=== Testing no-auth/transitionToAuth cluster ===');
mixedShardTest(noAuthOptions, transitionToX509allowTLS, true);
mixedShardTest(transitionToX509allowTLS, noAuthOptions, true);

print('=== Testing transitionToAuth/transitionToAuth cluster ===');
mixedShardTest(transitionToX509allowTLS, transitionToX509allowTLS, true);
