/**
 * Commands for change stream configuration testing.
 * Defines all command classes that perform operations on database/collection states.
 *
 * Note: Random is a global object provided by the MongoDB shell.
 */

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
 * TODO: SERVER-114857 - This command needs to know if the collection already exists.
 * The insertion behavior may differ depending on whether the collection is already created.
 */
class InsertDocCommand extends Command {
    constructor(dbName, collName, shardSet) {
        super(dbName, collName, shardSet);
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
 * Shard collection command.
 * TODO: SERVER-114857 - For now, we create a normal untracked collection instead of
 * actually sharding it. This allows subsequent operations (rename, drop, etc.) to work
 * on an existing collection. Full sharding setup will be addressed in SERVER-114857.
 */
class CreateShardedCollectionCommand extends Command {
    execute(connection) {
        // Create a normal untracked collection as a workaround
        // TODO: SERVER-114857 - This should actually shard the collection
        assert.commandWorked(connection.getDB(this.dbName).createCollection(this.collName));
    }

    toString() {
        return "CreateShardedCollectionCommand";
    }
}

/**
 * Unshard collection command.
 * TODO: SERVER-114857 - This is a no-op simulation for testing.
 * Since sharding is not actually set up, unsharding is also a no-op.
 */
class UnshardCollectionCommand extends Command {
    execute(connection) {
        // No-op: Unsharding simulation left blank since sharding is not actually set up.
        // The state machine transition still occurs correctly.
    }

    toString() {
        return "UnshardCollectionCommand";
    }
}

/**
 * Unified rename collection command.
 * Handles all rename scenarios based on configuration.
 */
class RenameCommand extends Command {
    constructor(dbName, collName, shardSet, targetShouldExist, crossDatabase) {
        super(dbName, collName, shardSet);
        // targetShouldExist is kept for distinguishing command types but not used in execution
        this.targetShouldExist = targetShouldExist;
        this.crossDatabase = crossDatabase;
    }

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

// Concrete rename command classes for backward compatibility.
class RenameToNonExistentSameDbCommand extends RenameCommand {
    constructor(dbName, collName, shardSet) {
        super(dbName, collName, shardSet, false, false);
    }
}

class RenameToExistentSameDbCommand extends RenameCommand {
    constructor(dbName, collName, shardSet) {
        super(dbName, collName, shardSet, true, false);
    }
}

class RenameToNonExistentDifferentDbCommand extends RenameCommand {
    constructor(dbName, collName, shardSet) {
        super(dbName, collName, shardSet, false, true);
    }
}

class RenameToExistentDifferentDbCommand extends RenameCommand {
    constructor(dbName, collName, shardSet) {
        super(dbName, collName, shardSet, true, true);
    }
}

/**
 * Reshard collection command.
 * TODO: SERVER-114857 - This is a no-op simulation for testing.
 * Resharding is complex and changes the shard key, which is not critical for
 * state machine testing. The collection remains in the sharded state.
 */
class ReshardCollectionCommand extends Command {
    execute(connection) {
        // No-op: Resharding simulation left blank as it's not critical for state machine testing.
        // The collection stays sharded, which is sufficient for the state machine model.
    }

    toString() {
        return "ReshardCollectionCommand";
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
        // sharding is not actually set up (SERVER-114857).
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
    CreateShardedCollectionCommand,
    CreateUnsplittableCollectionCommand,
    CreateUntrackedCollectionCommand,
    DropCollectionCommand,
    DropDatabaseCommand,
    RenameCommand,
    RenameToNonExistentSameDbCommand,
    RenameToExistentSameDbCommand,
    RenameToNonExistentDifferentDbCommand,
    RenameToExistentDifferentDbCommand,
    UnshardCollectionCommand,
    ReshardCollectionCommand,
    MoveCommandBase,
    MovePrimaryCommand,
    MoveCollectionCommand,
    MoveChunkCommand,
};
