/**
 * Provides a hook to check that indexes are consistent across the sharded cluster.
 *
 * The hook checks that for every collection, all the shards that own chunks for the
 * collection have the same indexes.
 */
"use strict";

ShardingTest.prototype.checkIndexesConsistentAcrossCluster = function() {
    if (jsTest.options().skipCheckingIndexesConsistentAcrossCluster) {
        print("Skipping index consistency check across the cluster");
        return;
    }

    /**
     * Returns true if the array contains the given BSON object.
     */
    function containsBSON(arr, targetObj) {
        for (const obj of arr) {
            if (bsonWoCompare(obj, targetObj) === 0) {
                return true;
            }
        }
        return false;
    }

    print("Checking consistency of indexes across the cluster");

    const mongos = new Mongo(this.s.host);
    const keyFile = this.keyFile;

    // TODO (SERVER-45017): Remove this check when v4.4 becomes last-stable.
    const isMixedVersion = this.isMixedVersionCluster();

    /**
     * Returns an array of config.collections docs for undropped collections.
     */
    function getCollDocs() {
        return mongos.getDB("config").collections.find({dropped: false}).toArray();
    }

    /**
     * Returns a function that returns an array of index docs for the namespace grouped
     * by shard.
     */
    function makeGetIndexDocsFunc(ns) {
        return () => {
            mongos.setReadPref("primary");
            if (isMixedVersion) {
                return mongos.getCollection(ns)
                    .aggregate([
                        {$indexStats: {}},
                        {$group: {_id: "$host", indexes: {$push: {key: "$key", name: "$name"}}}},
                        {$project: {_id: 0, host: "$_id", indexes: 1}}
                    ])
                    .toArray();
            }
            return mongos.getCollection(ns)
                .aggregate([
                    {$indexStats: {}},
                    {$group: {_id: "$shard", indexes: {$push: {spec: "$spec"}}}},
                    {$project: {_id: 0, shard: "$_id", indexes: 1}}
                ])
                .toArray();
        };
    }

    const collDocs = keyFile ? authutil.asCluster(mongos, keyFile, getCollDocs) : getCollDocs();
    for (const collDoc of collDocs) {
        const ns = collDoc._id;
        const getIndexDocsForNs = makeGetIndexDocsFunc(ns);
        print(`Checking that the indexes for ${ns} are consistent across shards...`);

        // Find the indexes on each shard. For example:
        // [{"shard" : "rs0",
        //   "indexes" : [{"spec" : {"v" : 2, "key" : {"_id" : 1}, "name" : "_id_"}},
        //                {"spec" : {"v" : 2, "key" : {"x" : 1}, "name" : "x_1"}}]},
        //  {"shard" : "rs1",
        //   "indexes" : [{"spec" : {"v" : 2, "key" : {"_id" :1}, "name" : "_id_"}}]}];
        const indexDocs =
            keyFile ? authutil.asCluster(mongos, keyFile, getIndexDocsForNs) : getIndexDocsForNs();

        if (indexDocs.length == 0) {
            print(`Found no indexes for ${ns}, skipping index consistency check`);
            continue;
        }

        // Find indexes that exist on all shards. For the example above:
        // [{"spec" : {"v" : 2, "key" : {"_id" : 1}, "name" : "_id_"}}];
        let consistentIndexes = indexDocs[0].indexes;
        for (let i = 1; i < indexDocs.length; i++) {
            consistentIndexes =
                consistentIndexes.filter(index => containsBSON(indexDocs[i].indexes, index));
        }

        // Find inconsistent indexes. For the example above:
        // {"rs0": [{"spec" : {"v" : 2, "key" : {"_id" : 1}, "name" : "_id_"}}], "rs1" : []};
        const inconsistentIndexesOnShard = {};
        let isConsistent = true;
        for (const indexDoc of indexDocs) {
            const inconsistentIndexes =
                indexDoc.indexes.filter(index => !containsBSON(consistentIndexes, index));
            inconsistentIndexesOnShard[isMixedVersion ? indexDoc.host : indexDoc.shard] =
                inconsistentIndexes;
            isConsistent = inconsistentIndexes.length === 0;
        }

        assert(isConsistent,
               `found inconsistent indexes for ${ns}: ${tojson(inconsistentIndexesOnShard)}`);
    }
};
