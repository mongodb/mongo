/**
 * Provides a hook to check that indexes are consistent across the sharded cluster.
 *
 * The hook checks that for every collection, all the shards that own chunks for the
 * collection have the same indexes.
 */
"use strict";

load("jstests/libs/check_cluster_index_consistency_helpers.js");  // For check implementation.

ShardingTest.prototype.checkIndexesConsistentAcrossCluster = function() {
    if (jsTest.options().skipCheckingIndexesConsistentAcrossCluster) {
        print("Skipping index consistency check across the cluster");
        return;
    }

    print("Checking consistency of indexes across the cluster");

    const mongos = new Mongo(this.s.host);
    mongos.fullOptions = this.s.fullOptions || {};
    mongos.setReadPref("primary");

    ClusterIndexConsistencyChecker.run(mongos, this.keyFile);
};
