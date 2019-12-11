/*
 * Utilities for checking indexes on shards.
 */
let ShardedIndexUtil = (function() {
    /*
     * Asserts that the shard has an index for the collection with the given index key.
     */
    let assertIndexExistsOnShard = function(shard, dbName, collName, targetIndexKey) {
        let res = shard.getDB(dbName).runCommand({listIndexes: collName});
        assert.commandWorked(res);

        let indexesOnShard = res.cursor.firstBatch;
        const isTargetIndex = (index) => bsonWoCompare(index.key, targetIndexKey) === 0;
        assert(indexesOnShard.some(isTargetIndex));
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
            assert.neq(0, bsonWoCompare(index.key, targetIndexKey));
        });
    };

    return {assertIndexExistsOnShard, assertIndexDoesNotExistOnShard};
})();
