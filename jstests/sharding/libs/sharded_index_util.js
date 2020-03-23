/*
 * Utilities for checking indexes on shards.
 */
var ShardedIndexUtil = (function() {
    /*
     * Asserts that the shard has an index for the collection with the given index key.
     */
    let assertIndexExistsOnShard = function(shard, dbName, collName, targetIndexKey) {
        let res = shard.getDB(dbName).runCommand({listIndexes: collName});
        assert.commandWorked(res);

        let indexesOnShard = res.cursor.firstBatch;
        const isTargetIndex = (index) => bsonWoCompare(index.key, targetIndexKey) === 0;
        assert(indexesOnShard.some(isTargetIndex),
               `expected shard ${shard.shardName} to have the index ${tojson(targetIndexKey)}`);
    };

    /*
     * Asserts that the shard does not have an index for the collection with the given index key.
     */
    let assertIndexDoesNotExistOnShard = function(shard, dbName, collName, targetIndexKey) {
        let res = shard.getDB(dbName).runCommand({listIndexes: collName});
        if (!res.ok && res.code === ErrorCodes.NamespaceNotFound) {
            // The collection does not exist on the shard, neither does the target index.
            return;
        }
        assert.commandWorked(res);

        let indexesOnShard = res.cursor.firstBatch;
        indexesOnShard.forEach(function(index) {
            assert.neq(0,
                       bsonWoCompare(index.key, targetIndexKey),
                       `expected shard ${shard.shardName} to not have the index ${
                           tojson(targetIndexKey)}`);
        });
    };

    /*
     * Returns true if the array contains the given BSON object.
     */
    let containsBSON = function(arr, targetObj) {
        for (const obj of arr) {
            if (bsonWoCompare(obj, targetObj) === 0) {
                return true;
            }
        }
        return false;
    };

    let getPerShardIndexes = function(coll) {
        return coll
            .aggregate(
                [
                    {$indexStats: {}},
                    {$group: {_id: "$shard", indexes: {$push: {spec: "$spec"}}}},
                    {$project: {_id: 0, shard: "$_id", indexes: 1}}
                ],
                {readConcern: {level: "local"}})
            .toArray();
    };

    /*
     *  Given an array of objects which contain a shard name and a list of indexes belonging to
     *  that shard (see example below), returns a map of shards  to a list of their inconsistent
     *  indexes, that is, any indexes which do not exist on all other shards.
     *
     *  For example:
     *  [{"shard" : "rs0",
     *      "indexes" : [{"spec" : {"v" : 2, "key" : {"_id" : 1}, "name" : "_id_"}},
     *                   {"spec" : {"v" : 2, "key" : {"x" : 1}, "name" : "x_1"}}]},
     *  {"shard" : "rs1",
     *      "indexes" : [{"spec" : {"v" : 2, "key" : {"_id" :1}, "name" : "_id_"}}]}];
     */
    let findInconsistentIndexesAcrossShards = function(indexDocs) {
        // Find indexes that exist on all shards. For the example above:
        // [{"spec" : {"v" : 2, "key" : {"_id" : 1}, "name" : "_id_"}}];
        let consistentIndexes = indexDocs[0].indexes;
        for (let i = 1; i < indexDocs.length; i++) {
            consistentIndexes =
                consistentIndexes.filter(index => this.containsBSON(indexDocs[i].indexes, index));
        }

        // Find inconsistent indexes. For the example above:
        // {"rs0": [{"spec" : {"v" : 2, "key" : {"x" : 1}, "name" : "x_1"}}], "rs1" : []};
        const inconsistentIndexesOnShard = {};
        for (const indexDoc of indexDocs) {
            const inconsistentIndexes =
                indexDoc.indexes.filter(index => !this.containsBSON(consistentIndexes, index));
            inconsistentIndexesOnShard[indexDoc.shard] = inconsistentIndexes;
        }

        return inconsistentIndexesOnShard;
    };

    return {
        assertIndexExistsOnShard,
        assertIndexDoesNotExistOnShard,
        containsBSON,
        getPerShardIndexes,
        findInconsistentIndexesAcrossShards
    };
})();
