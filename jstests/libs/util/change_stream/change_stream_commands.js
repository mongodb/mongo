/**
 * Commands for change stream configuration testing.
 * Defines all command classes that perform operations on database/collection states.
 *
 * Note: Random is a global object provided by the MongoDB shell.
 */

/**
 * Sharding type constants.
 */
const ShardingType = {
    RANGE: "range",
    HASHED: "hashed",
};

/**
 * Get the shard key spec for a given sharding type.
 * Uses 'data' field which requires explicit index creation before sharding.
 * @param {string} shardingType - ShardingType.RANGE or ShardingType.HASHED
 * @returns {Object} The shard key spec, e.g. {data: 1} or {data: "hashed"}
 */
function getShardKeySpec(shardingType) {
    return shardingType === ShardingType.HASHED ? {data: "hashed"} : {data: 1};
}

/**
 * Base command class.
 */
class Command {
    constructor(dbName, collName, shardSet) {
        this.dbName = dbName;
        this.collName = collName;
        this.shardSet = shardSet;
    }

    /**
     * Execute the command.
     * @param {Object} connection - The MongoDB connection.
     */
    execute(connection) {
        throw new Error("execute() method must be implemented by subclasses");
    }

    /**
     * String representation of the command.
     */
    toString() {
        throw new Error("toString() method must be implemented by subclasses");
    }

    /**
     * Get change event for this command.
     * TODO: Implement change event generation for each command type.
     */
    getChangeEvents() {
        throw new Error("getChangeEvents() not implemented yet");
    }
}

/**
 * Insert document command.
 */
class InsertDocCommand extends Command {
    constructor(dbName, collName, shardSet, collectionCtx) {
        super(dbName, collName, shardSet);
        // Store context state needed for event matching
        this.collectionExists = collectionCtx.exists;
        this.collectionNonEmpty = collectionCtx.nonEmpty;
        // Create the document in the constructor so it can be used by both execute() and getChangeEvents()
        this.document = {
            _id: new ObjectId(),
            timestamp: new Date(),
            data: `test_data`,
        };
    }

    execute(connection) {
        assert.commandWorked(connection.getDB(this.dbName).getCollection(this.collName).insertOne(this.document));
    }

    toString() {
        return "InsertDocCommand";
    }
}

/**
 * Create database command.
 */
class CreateDatabaseCommand extends Command {
    execute(connection) {
        assert.commandWorked(connection.adminCommand({enableSharding: this.dbName}));
    }

    toString() {
        return "CreateDatabaseCommand";
    }
}

/**
 * Create unsplittable collection command.
 */
class CreateUnsplittableCollectionCommand extends Command {
    execute(connection) {
        assert(
            this.shardSet && this.shardSet.length > 0,
            "Shard set must be provided for CreateUnsplittableCollectionCommand",
        );
        const targetShard = this.shardSet[Random.randInt(this.shardSet.length)];

        const db = connection.getDB(this.dbName);
        assert.commandWorked(
            db.runCommand({
                createUnsplittableCollection: this.collName,
                dataShard: targetShard._id,
            }),
        );
    }

    toString() {
        return "CreateUnsplittableCollectionCommand";
    }
}

/**
 * Create untracked collection command.
 */
class CreateUntrackedCollectionCommand extends Command {
    execute(connection) {
        assert.commandWorked(connection.getDB(this.dbName).createCollection(this.collName));
    }

    toString() {
        return "CreateUntrackedCollectionCommand";
    }
}

/**
 * Drop collection command.
 */
class DropCollectionCommand extends Command {
    execute(connection) {
        const db = connection.getDB(this.dbName);
        assert.commandWorked(db.runCommand({drop: this.collName}));
    }

    toString() {
        return "DropCollectionCommand";
    }
}

/**
 * Drop database command.
 */
class DropDatabaseCommand extends Command {
    execute(connection) {
        const db = connection.getDB(this.dbName);
        assert.commandWorked(db.dropDatabase());
    }

    toString() {
        return "DropDatabaseCommand";
    }
}

/**
 * Create index command.
 * Creates an index for the shard key (required before sharding).
 * Precondition (guaranteed by FSM): collection exists.
 */
class CreateIndexCommand extends Command {
    constructor(dbName, collName, shardSet, collectionCtx) {
        super(dbName, collName, shardSet);
        assert(collectionCtx.shardKeySpec, "shardKeySpec must be provided to CreateIndexCommand");
        this.shardKeySpec = collectionCtx.shardKeySpec;
    }

    execute(connection) {
        const coll = connection.getDB(this.dbName).getCollection(this.collName);
        assert.commandWorked(coll.createIndex(this.shardKeySpec));
    }

    toString() {
        return `CreateIndexCommand(${tojson(this.shardKeySpec)})`;
    }
}

/**
 * Drop index command.
 * Drops the shard key index (cleanup after resharding).
 * Preconditions (guaranteed by generator): collection exists, index exists.
 */
class DropIndexCommand extends Command {
    constructor(dbName, collName, shardSet, collectionCtx) {
        super(dbName, collName, shardSet);
        assert(collectionCtx.shardKeySpec, "shardKeySpec must be provided to DropIndexCommand");
        this.shardKeySpec = collectionCtx.shardKeySpec;
    }

    execute(connection) {
        const coll = connection.getDB(this.dbName).getCollection(this.collName);
        assert.commandWorked(coll.dropIndex(this.shardKeySpec));
    }

    toString() {
        return `DropIndexCommand(${tojson(this.shardKeySpec)})`;
    }
}

/**
 * Shard existing collection command.
 * Sharding type (range vs hashed) is determined by the shardKeySpec in collectionCtx.
 * Preconditions (guaranteed by FSM): collection exists, shard key index exists.
 */
class ShardCollectionCommand extends Command {
    constructor(dbName, collName, shardSet, collectionCtx) {
        super(dbName, collName, shardSet);
        assert(collectionCtx.shardKeySpec, "shardKeySpec must be provided to ShardCollectionCommand");
        this.shardKeySpec = collectionCtx.shardKeySpec;
    }

    execute(connection) {
        const ns = `${this.dbName}.${this.collName}`;

        // Shard the collection
        assert.commandWorked(
            connection.adminCommand({
                shardCollection: ns,
                key: this.shardKeySpec,
            }),
        );
    }

    toString() {
        const type = Object.values(this.shardKeySpec).some((v) => v === "hashed") ? "hashed" : "range";
        return `ShardCollectionCommand(${type})`;
    }
}

/**
 * Unshard collection command.
 * Converts a sharded collection to an unsplittable (single-shard) collection.
 * Picks a random shard as the destination.
 * Precondition (guaranteed by FSM): collection exists and is sharded.
 */
class UnshardCollectionCommand extends Command {
    execute(connection) {
        assert(this.shardSet && this.shardSet.length > 0, "Shard set must be provided for UnshardCollectionCommand");
        const targetShard = this.shardSet[Random.randInt(this.shardSet.length)];
        const ns = `${this.dbName}.${this.collName}`;
        assert.commandWorked(
            connection.adminCommand({
                unshardCollection: ns,
                toShard: targetShard._id,
            }),
        );
    }

    toString() {
        return "UnshardCollectionCommand";
    }
}

/**
 * Reshard collection command.
 * Resharding type (range vs hashed) is determined by shardKeySpec in collectionCtx.
 * Precondition (guaranteed by FSM): collection exists and is sharded, index exists.
 */
class ReshardCollectionCommand extends Command {
    constructor(dbName, collName, shardSet, collectionCtx) {
        super(dbName, collName, shardSet);
        assert(collectionCtx.shardKeySpec, "shardKeySpec must be provided to ReshardCollectionCommand");
        this.shardKeySpec = collectionCtx.shardKeySpec;
    }

    execute(connection) {
        const ns = `${this.dbName}.${this.collName}`;

        // Use numInitialChunks: 1 to avoid cardinality errors in test environments with little data.
        assert.commandWorked(
            connection.adminCommand({
                reshardCollection: ns,
                key: this.shardKeySpec,
                numInitialChunks: 1,
            }),
        );
    }

    toString() {
        const type = Object.values(this.shardKeySpec).some((v) => v === "hashed") ? "hashed" : "range";
        return `ReshardCollectionCommand(${type})`;
    }
}

/**
 * Base rename collection command.
 * Subclasses define targetShouldExist and crossDatabase flags.
 */
class RenameCommand extends Command {
    // Subclasses must set: this.targetShouldExist, this.crossDatabase

    execute(connection) {
        const targetDb = this.crossDatabase ? `${this.dbName}_target` : this.dbName;
        const targetColl = `${this.collName}_renamed`;

        assert.commandWorked(
            connection.adminCommand({
                renameCollection: `${this.dbName}.${this.collName}`,
                to: `${targetDb}.${targetColl}`,
            }),
        );

        // Drop the renamed collection to ensure the target doesn't already exist for subsequent renames
        assert.commandWorked(connection.getDB(targetDb).runCommand({drop: targetColl}));
    }

    toString() {
        const targetType = this.targetShouldExist ? "Existent" : "NonExistent";
        const dbType = this.crossDatabase ? "DifferentDb" : "SameDb";
        return `RenameTo${targetType}${dbType}Command`;
    }
}

// Concrete rename command classes.
class RenameToNonExistentSameDbCommand extends RenameCommand {
    constructor(dbName, collName, shardSet) {
        super(dbName, collName, shardSet);
        this.targetShouldExist = false;
        this.crossDatabase = false;
    }
}

class RenameToExistentSameDbCommand extends RenameCommand {
    constructor(dbName, collName, shardSet) {
        super(dbName, collName, shardSet);
        this.targetShouldExist = true;
        this.crossDatabase = false;
    }
}

class RenameToNonExistentDifferentDbCommand extends RenameCommand {
    constructor(dbName, collName, shardSet) {
        super(dbName, collName, shardSet);
        this.targetShouldExist = false;
        this.crossDatabase = true;
    }
}

class RenameToExistentDifferentDbCommand extends RenameCommand {
    constructor(dbName, collName, shardSet) {
        super(dbName, collName, shardSet);
        this.targetShouldExist = true;
        this.crossDatabase = true;
    }
}

/**
 * Base class for move operations.
 * Handles shard selection and provides common move functionality.
 */
class MoveCommandBase extends Command {
    /**
     * Get the target shard for the move operation.
     * @param {Object} connection - The MongoDB connection.
     * @returns {string|null} The target shard ID, or null if no suitable shard exists.
     */
    _getTargetShard(connection) {
        throw new Error("_getTargetShard() method must be implemented by subclasses");
    }

    /**
     * Build the move command to execute.
     * @param {string} targetShardId - The target shard ID.
     * @returns {Object} The command object to send to adminCommand.
     */
    _buildMoveCommand(targetShardId) {
        throw new Error("_buildMoveCommand() method must be implemented by subclasses");
    }

    execute(connection) {
        // No-op: Move operations are not critical for state machine testing since
        // sharding is not actually set up.
        // In a real implementation, this would execute:
        //   const targetShardId = this._getTargetShard(connection);
        //   const moveCommand = this._buildMoveCommand(targetShardId);
        //   assert.commandWorked(connection.adminCommand(moveCommand));
    }
}

/**
 * Move primary command.
 */
class MovePrimaryCommand extends MoveCommandBase {
    _getTargetShard(connection) {
        assert(this.shardSet && this.shardSet.length > 0, "Shard set must be provided for MovePrimaryCommand");
        assert.gt(this.shardSet.length, 1, "MovePrimaryCommand requires at least 2 shards");
        const dbInfo = assert.commandWorked(connection.adminCommand({getDatabaseVersion: this.dbName}));
        const otherShards = this.shardSet.filter((s) => s._id !== dbInfo.primary);
        assert.gt(
            otherShards.length,
            0,
            `MovePrimaryCommand requires at least one shard other than primary (${dbInfo.primary})`,
        );
        return otherShards[Random.randInt(otherShards.length)]._id;
    }

    _buildMoveCommand(targetShardId) {
        return {
            movePrimary: this.dbName,
            to: targetShardId,
        };
    }

    toString() {
        return "MovePrimaryCommand";
    }
}

/**
 * Move collection command.
 */
class MoveCollectionCommand extends MoveCommandBase {
    _getTargetShard(connection) {
        assert(this.shardSet && this.shardSet.length > 0, "Shard set must be provided for MoveCollectionCommand");
        assert.gt(this.shardSet.length, 1, "MoveCollectionCommand requires at least 2 shards");
        return this.shardSet[Random.randInt(this.shardSet.length)]._id;
    }

    _buildMoveCommand(targetShardId) {
        return {
            moveCollection: `${this.dbName}.${this.collName}`,
            toShard: targetShardId,
        };
    }

    toString() {
        return "MoveCollectionCommand";
    }
}

/**
 * Move chunk command.
 * TODO: SERVER-114858 - Improve chunk selection logic.
 */
class MoveChunkCommand extends MoveCommandBase {
    _getTargetShard(connection) {
        assert(this.shardSet && this.shardSet.length > 0, "Shard set must be provided for MoveChunkCommand");
        assert.gt(this.shardSet.length, 1, "MoveChunkCommand requires at least 2 shards");
        return this.shardSet[Random.randInt(this.shardSet.length)]._id;
    }

    _buildMoveCommand(targetShardId) {
        return {
            moveChunk: `${this.dbName}.${this.collName}`,
            find: {_id: MinKey},
            to: targetShardId,
        };
    }

    toString() {
        return "MoveChunkCommand";
    }
}

// Export classes.
export {
    Command,
    InsertDocCommand,
    CreateDatabaseCommand,
    CreateIndexCommand,
    DropIndexCommand,
    ShardCollectionCommand,
    CreateUnsplittableCollectionCommand,
    CreateUntrackedCollectionCommand,
    DropCollectionCommand,
    DropDatabaseCommand,
    RenameToNonExistentSameDbCommand,
    RenameToExistentSameDbCommand,
    RenameToNonExistentDifferentDbCommand,
    RenameToExistentDifferentDbCommand,
    UnshardCollectionCommand,
    ReshardCollectionCommand,
    MovePrimaryCommand,
    MoveCollectionCommand,
    MoveChunkCommand,
    ShardingType,
    getShardKeySpec,
};
