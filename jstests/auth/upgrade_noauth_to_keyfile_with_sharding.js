// Tests access control upgrade on a sharded cluster
// The purpose is to verify the connectivity between mongos, config server, and the shards
// @tags: [requires_sharding]

import {KEYFILE, mixedShardTest} from "jstests/ssl/libs/ssl_helpers.js";

// IndexConsistencyCheck requires auth which ttA/ttA fails at.
TestData.skipCheckingIndexesConsistentAcrossCluster = true;
TestData.skipCheckOrphans = true;
TestData.skipCheckShardFilteringMetadata = true;
TestData.skipCheckRoutingTableConsistency = true;

// Disable auth explicitly
let noAuthOptions = {noauth: ""};
let transitionToAuthOptions = {clusterAuthMode: "keyFile", keyFile: KEYFILE, transitionToAuth: ""};
let keyFileOptions = {clusterAuthMode: "keyFile", keyFile: KEYFILE};

print("=== Testing no-auth/transitionToAuth cluster ===");
mixedShardTest(noAuthOptions, transitionToAuthOptions, true);
mixedShardTest(transitionToAuthOptions, noAuthOptions, true);

print("=== Testing transitionToAuth/transitionToAuth cluster ===");
mixedShardTest(transitionToAuthOptions, transitionToAuthOptions, true);

print("=== Testing transitionToAuth/keyFile cluster ===");
mixedShardTest(keyFileOptions, transitionToAuthOptions, true);
mixedShardTest(transitionToAuthOptions, keyFileOptions, true);

print("=== Testing no-auth/keyFile cluster fails ===");
mixedShardTest(noAuthOptions, keyFileOptions, false);
mixedShardTest(keyFileOptions, noAuthOptions, false);
