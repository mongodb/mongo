/**
 * Parameters for ShardingCommandGenerator.
 * Simple struct encapsulating database, collection, and shard information.
 */
class ShardingCommandGeneratorParams {
    /**
     * @param {string} dbName - Database name
     * @param {string} collName - Collection name
     * @param {Array} shardSet - Array of shard objects
     */
    constructor(dbName, collName, shardSet) {
        this.dbName = dbName;
        this.collName = collName;
        this.shardSet = shardSet;
    }

    getDbName() {
        return this.dbName;
    }

    getCollName() {
        return this.collName;
    }

    getShardSet() {
        return this.shardSet;
    }
}

export {ShardingCommandGeneratorParams};
