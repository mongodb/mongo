/**
 * Provides a hook to check that indexes are consistent across the sharded cluster.
 *
 * The hook checks that for every collection, all the shards that own chunks for the
 * collection have the same indexes.
 */
"use strict";

load("jstests/sharding/libs/sharded_index_util.js");  // for findInconsistentIndexesAcrossShards

ShardingTest.prototype.checkIndexesConsistentAcrossCluster = function() {
    if (jsTest.options().skipCheckingIndexesConsistentAcrossCluster) {
        print("Skipping index consistency check across the cluster");
        return;
    }

    print("Checking consistency of indexes across the cluster");

    const mongos = new Mongo(this.s.host);
    mongos.fullOptions = this.s.fullOptions || {};
    mongos.forceReadMode("commands");
    mongos.setReadPref("primary");

    const keyFile = this.keyFile;

    /**
     * Returns an array of config.collections docs for undropped collections.
     */
    function getCollDocs() {
        return mongos.getDB("config")
            .collections.find({dropped: false})
            .readConcern("local")
            .toArray();
    }

    /**
     * Returns a function that returns an array of index docs for the namespace grouped
     * by shard.
     */
    function makeGetIndexDocsFunc(ns) {
        return () => {
            return ShardedIndexUtil.getPerShardIndexes(mongos.getCollection(ns));
        };
    }

    const requiresAuth = keyFile || (mongos.fullOptions.clusterAuthMode === 'x509');
    const collDocs =
        requiresAuth ? authutil.asCluster(mongos, keyFile, getCollDocs) : getCollDocs();
    for (const collDoc of collDocs) {
        const ns = collDoc._id;
        const getIndexDocsForNs = makeGetIndexDocsFunc(ns);
        print(`Checking that the indexes for ${ns} are consistent across shards...`);

        const indexDocs = requiresAuth ? authutil.asCluster(mongos, keyFile, getIndexDocsForNs)
                                       : getIndexDocsForNs();

        if (indexDocs.length == 0) {
            print(`Found no indexes for ${ns}, skipping index consistency check`);
            continue;
        }

        const inconsistentIndexes = ShardedIndexUtil.findInconsistentIndexesAcrossShards(indexDocs);

        for (const shard in inconsistentIndexes) {
            const shardInconsistentIndexes = inconsistentIndexes[shard];
            assert(shardInconsistentIndexes.length === 0,
                   `found inconsistent indexes for ${ns}: ${tojson(inconsistentIndexes)}`);
        }
    }
};
