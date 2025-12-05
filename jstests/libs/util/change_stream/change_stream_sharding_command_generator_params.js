/**
 * Parameters for ShardingCommandGenerator.
 * Encapsulates database, collection, and shard information.
 */
class ShardingCommandGeneratorParams {
    constructor(dbName, collName, shardSet) {
        this.dbName = dbName;
        this.collName = collName;
        this.shardSet = shardSet;
    }

    /**
     * Get the database name.
     * @returns {string} The database name.
     */
    getDbName() {
        return this.dbName;
    }

    /**
     * Get the collection name.
     * @returns {string} The collection name.
     */
    getCollName() {
        return this.collName;
    }

    /**
     * Get the shard set.
     * @returns {Array} Array of shard objects.
     */
    getShardSet() {
        return this.shardSet;
    }
}

export {ShardingCommandGeneratorParams};
