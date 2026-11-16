/**
 * Generates commands that explore all states and transitions in a test model.
 * Uses the jstest shell's Random implementation for reproducibility.
 * Requires an explicit seed to ensure deterministic behavior.
 */
import {FsmCommandGenerator} from "jstests/libs/util/change_stream/change_stream_fsm_command_generator.js";
import {ShardingAction as Action} from "jstests/libs/util/change_stream/change_stream_sharding_action.js";
import {State} from "jstests/libs/util/change_stream/change_stream_sharding_state.js";
import {ShardingCommandGeneratorParams} from "jstests/libs/util/change_stream/change_stream_sharding_command_generator_params.js";
import {
    InsertDocCommand,
    CreateDatabaseCommand,
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
    CreateIndexCommand,
    DropIndexCommand,
    ShardingType,
    getShardKeySpec,
} from "jstests/libs/util/change_stream/change_stream_commands.js";

class ShardingCommandGenerator extends FsmCommandGenerator {
    // Maps action IDs to command classes.
    static actionToCommandClass = {
        [Action.INSERT_DOC]: InsertDocCommand,
        [Action.CREATE_DATABASE]: CreateDatabaseCommand,
        [Action.CREATE_SHARDED_COLLECTION_RANGE]: ShardCollectionCommand,
        [Action.CREATE_SHARDED_COLLECTION_HASHED]: ShardCollectionCommand,
        [Action.SHARD_COLLECTION_RANGE]: ShardCollectionCommand,
        [Action.SHARD_COLLECTION_HASHED]: ShardCollectionCommand,
        [Action.RESHARD_COLLECTION_TO_RANGE]: ReshardCollectionCommand,
        [Action.RESHARD_COLLECTION_TO_HASHED]: ReshardCollectionCommand,
        [Action.CREATE_UNSPLITTABLE_COLLECTION]: CreateUnsplittableCollectionCommand,
        [Action.CREATE_UNTRACKED_COLLECTION]: CreateUntrackedCollectionCommand,
        [Action.DROP_COLLECTION]: DropCollectionCommand,
        [Action.DROP_DATABASE]: DropDatabaseCommand,
        [Action.RENAME_TO_NON_EXISTENT_SAME_DB]: RenameToNonExistentSameDbCommand,
        [Action.RENAME_TO_EXISTENT_SAME_DB]: RenameToExistentSameDbCommand,
        [Action.RENAME_TO_NON_EXISTENT_DIFFERENT_DB]: RenameToNonExistentDifferentDbCommand,
        [Action.RENAME_TO_EXISTENT_DIFFERENT_DB]: RenameToExistentDifferentDbCommand,
        [Action.UNSHARD_COLLECTION]: UnshardCollectionCommand,
        [Action.MOVE_PRIMARY]: MovePrimaryCommand,
        [Action.MOVE_COLLECTION]: MoveCollectionCommand,
        [Action.MOVE_CHUNK]: MoveChunkCommand,
    };

    /**
     * Create a command instance for a given action ID.
     * @param {number} action - The action ID.
     * @param {ShardingCommandGeneratorParams} params - The generator parameters.
     * @param {Object} collectionCtx - The collection context with sharding config.
     * @param {Object} targetShardKey - Optional target shard key for sharding operations.
     * @returns {Command} The command instance.
     */
    createCommand(action, params, collectionCtx, targetShardKey = null) {
        const CommandClass = ShardingCommandGenerator.actionToCommandClass[action];
        assert(CommandClass !== undefined, `No command class found for action ${action}`);

        const baseOpts = {
            dbName: params.getDbName(),
            collName: params.getCollName(),
            shardSet: params.getShardSet(),
            collectionCtx: {...collectionCtx},
        };

        // Commands that need the target shard key as a separate parameter.
        if (CommandClass === ShardCollectionCommand || CommandClass === ReshardCollectionCommand) {
            assert(targetShardKey, `${CommandClass.name} requires targetShardKey`);
            return new CommandClass({...baseOpts, shardKey: targetShardKey});
        }

        return new CommandClass(baseOpts);
    }

    /**
     * Create an empty collection context (collection does not exist).
     * shardKeySpec represents the collection's CURRENT shard key (state).
     */
    static _emptyCollectionCtx() {
        return {
            exists: false,
            nonEmpty: false,
            shardKeySpec: null, // Collection's current shard key (null if not sharded)
            isUnsplittable: false, // True if the collection is unsplittable (tracked placement)
        };
    }

    /**
     * Initialize collection context based on the starting state.
     * collectionCtx tracks: exists, nonEmpty, shardKeySpec, isUnsplittable.
     * shardKeySpec represents the collection's CURRENT shard key.
     * Note: Even if starting from a collection-present state, we assume the collection
     * is empty (nonEmpty: false) since we don't know its actual contents.
     */
    _initialContextForState(state) {
        const ctx = ShardingCommandGenerator._emptyCollectionCtx();
        const set = (overrides) => Object.assign(ctx, overrides);

        switch (state) {
            case State.DATABASE_ABSENT:
            case State.DATABASE_PRESENT_COLLECTION_ABSENT:
                break;
            case State.COLLECTION_PRESENT_UNTRACKED:
                set({exists: true});
                break;
            case State.COLLECTION_PRESENT_UNSPLITTABLE:
                set({exists: true, isUnsplittable: true});
                break;
            case State.COLLECTION_PRESENT_SHARDED_RANGE:
                set({exists: true, shardKeySpec: getShardKeySpec(ShardingType.RANGE)});
                break;
            case State.COLLECTION_PRESENT_SHARDED_HASHED:
                set({exists: true, shardKeySpec: getShardKeySpec(ShardingType.HASHED)});
                break;
            default:
                assert(false, `Unknown state: ${state}. Add handling for this state.`);
        }

        return ctx;
    }

    /**
     * Update collection context after executing an action.
     * Sharding type is determined by the action itself.
     * Updates shardKeySpec to reflect the collection's new shard key after the action.
     */
    _updateContextForAction(action, ctx) {
        const set = (overrides) => Object.assign(ctx, overrides);
        const rangeKey = getShardKeySpec(ShardingType.RANGE);
        const hashedKey = getShardKeySpec(ShardingType.HASHED);

        switch (action) {
            case Action.INSERT_DOC:
                set({exists: true, nonEmpty: true});
                break;
            case Action.CREATE_SHARDED_COLLECTION_RANGE:
            case Action.SHARD_COLLECTION_RANGE:
                set({exists: true, shardKeySpec: rangeKey, isUnsplittable: false});
                break;
            case Action.CREATE_SHARDED_COLLECTION_HASHED:
            case Action.SHARD_COLLECTION_HASHED:
                // Hashed sharding with presplitHashedZones distributes chunks across all shards.
                set({
                    exists: true,
                    shardKeySpec: hashedKey,
                    isUnsplittable: false,
                });
                break;
            case Action.RESHARD_COLLECTION_TO_RANGE:
                assert(ctx.shardKeySpec, "Reshard requires existing shard key");
                set({shardKeySpec: rangeKey, isUnsplittable: false});
                break;
            case Action.RESHARD_COLLECTION_TO_HASHED:
                assert(ctx.shardKeySpec, "Reshard requires existing shard key");
                set({shardKeySpec: hashedKey, isUnsplittable: false});
                break;
            case Action.CREATE_UNSPLITTABLE_COLLECTION:
                set({
                    exists: true,
                    nonEmpty: false,
                    isUnsplittable: true,
                    shardKeySpec: null,
                });
                break;
            case Action.CREATE_UNTRACKED_COLLECTION:
                set({
                    exists: true,
                    nonEmpty: false,
                    isUnsplittable: false,
                    shardKeySpec: null,
                });
                break;
            case Action.UNSHARD_COLLECTION:
                assert(ctx.shardKeySpec, "Unshard requires existing shard key");
                set({shardKeySpec: null, isUnsplittable: true});
                break;
            case Action.MOVE_COLLECTION:
                set({isUnsplittable: true});
                break;
            case Action.MOVE_CHUNK:
            case Action.MOVE_PRIMARY:
            case Action.CREATE_DATABASE:
                break;
            case Action.DROP_COLLECTION:
            case Action.RENAME_TO_NON_EXISTENT_SAME_DB:
            case Action.RENAME_TO_EXISTENT_SAME_DB:
            case Action.RENAME_TO_NON_EXISTENT_DIFFERENT_DB:
            case Action.RENAME_TO_EXISTENT_DIFFERENT_DB:
            case Action.DROP_DATABASE:
                set(ShardingCommandGenerator._emptyCollectionCtx());
                break;
            default:
                assert(false, `Unhandled action in _updateCollectionCtxForAction: ${action}`);
        }
    }

    // Maps actions to their sharding type (for determining shard key spec).
    static actionToShardingType = {
        [Action.CREATE_SHARDED_COLLECTION_RANGE]: ShardingType.RANGE,
        [Action.SHARD_COLLECTION_RANGE]: ShardingType.RANGE,
        [Action.RESHARD_COLLECTION_TO_RANGE]: ShardingType.RANGE,
        [Action.CREATE_SHARDED_COLLECTION_HASHED]: ShardingType.HASHED,
        [Action.SHARD_COLLECTION_HASHED]: ShardingType.HASHED,
        [Action.RESHARD_COLLECTION_TO_HASHED]: ShardingType.HASHED,
    };

    // Actions that require shard key index to exist before execution.
    // NOTE: CREATE_SHARDED_COLLECTION_* is NOT included because shardCollection on a
    // non-existent collection implicitly creates both the collection and the shard key index.
    static actionsRequiringIndex = new Set([
        Action.SHARD_COLLECTION_RANGE,
        Action.SHARD_COLLECTION_HASHED,
        Action.RESHARD_COLLECTION_TO_RANGE,
        Action.RESHARD_COLLECTION_TO_HASHED,
    ]);

    // Actions that require dropping the old shard key index after execution.
    static actionsRequiringIndexCleanup = new Set([
        Action.RESHARD_COLLECTION_TO_RANGE,
        Action.RESHARD_COLLECTION_TO_HASHED,
        Action.UNSHARD_COLLECTION,
    ]);

    /**
     * Hook: the command(s) for a given action. Some actions require prerequisite
     * commands (e.g., index creation before sharding) — all appended by _appendAction.
     */
    _commandsForAction(action, params, context) {
        const out = [];
        this._appendAction(out, action, params, context);
        return out;
    }

    _appendAction(commands, action, params, collectionCtx) {
        // Step 1: Determine target shard key from action mapping.
        // This is the shard key for the OPERATION, not the collection's current state.
        const shardingType = ShardingCommandGenerator.actionToShardingType[action];
        const targetShardKey = shardingType ? getShardKeySpec(shardingType) : null;

        // Step 2: Build collection context copy.
        // collectionCtx.shardKeySpec represents the collection's CURRENT shard key (state).
        const ctx = {...collectionCtx};

        // Step 3: Pre-commands.

        // For actions requiring index on non-existent collection: create sharded collection directly.
        // ShardCollectionCommand implicitly creates collection + shard key index.
        // This avoids the redundant path: CreateUntrackedCollection -> CreateIndex -> ShardCollection
        if (ShardingCommandGenerator.actionsRequiringIndex.has(action) && !collectionCtx.exists) {
            // Only SHARD_COLLECTION_* actions can reach here when collection doesn't exist.
            // RESHARD_COLLECTION_* requires an existing sharded collection (enforced by state machine).
            // We return early because ShardCollectionCommand handles everything (create + index + shard).
            assert(
                action === Action.SHARD_COLLECTION_RANGE ||
                    action === Action.SHARD_COLLECTION_HASHED,
                `Unexpected action ${action} on non-existent collection - only SHARD_COLLECTION_* allowed`,
            );

            const shardCtx = {...ctx, exists: false};
            commands.push(
                new ShardCollectionCommand({
                    dbName: params.getDbName(),
                    collName: params.getCollName(),
                    shardSet: params.getShardSet(),
                    collectionCtx: shardCtx,
                    shardKey: targetShardKey,
                }),
            );
            // Update context - collection is now sharded
            ctx.exists = true;
            ctx.shardKeySpec = targetShardKey;
            return; // Skip index creation and main command - already done
        }

        // Create shard key index if action requires it (collection must already exist).
        if (ShardingCommandGenerator.actionsRequiringIndex.has(action)) {
            commands.push(
                new CreateIndexCommand({
                    dbName: params.getDbName(),
                    collName: params.getCollName(),
                    shardSet: params.getShardSet(),
                    indexSpec: targetShardKey,
                }),
            );
        }
        // NOTE: We intentionally do NOT drop indexes before dropping collections.
        // While MongoDB silently removes indexes when dropping a collection (no 'dropIndexes' events),
        // we already get 'dropIndexes' coverage from the post-reshard/unshard cleanup (Step 5).
        // Explicitly dropping here would be redundant and add unnecessary complexity.
        // Drop collection before dropping database (simplifies change event matching).
        if (action === Action.DROP_DATABASE && collectionCtx.exists) {
            // NOTE: We do NOT drop indexes here - see comment above about dropIndexes coverage.
            // DropCollectionCommand defaults to {exists: true}, which is what we want here
            // (we only enter this branch when ctx.exists is true).
            commands.push(
                new DropCollectionCommand({
                    dbName: params.getDbName(),
                    collName: params.getCollName(),
                    shardSet: params.getShardSet(),
                }),
            );
            // After dropping collection, update context for DropDatabaseCommand to reflect that collection no longer exists.
            ctx.exists = false;
        }

        // Step 4: Main command.
        commands.push(this.createCommand(action, params, ctx, targetShardKey));

        // Step 5: Post-commands.
        // Drop old shard key index AFTER resharding/unsharding.
        // For reshard: MongoDB won't allow dropping while still using it.
        // For unshard: drop the old shard key index since collection is no longer sharded.
        if (ShardingCommandGenerator.actionsRequiringIndexCleanup.has(action)) {
            const oldShardKey = collectionCtx.shardKeySpec;
            // For reshard: only drop old index if it's different from the new one.
            // For unshard: always drop the old shard key index (targetShardKey is null).
            const shouldDropIndex =
                action === Action.UNSHARD_COLLECTION ||
                bsonWoCompare(oldShardKey, targetShardKey) !== 0;
            if (shouldDropIndex && oldShardKey) {
                commands.push(
                    new DropIndexCommand({
                        dbName: params.getDbName(),
                        collName: params.getCollName(),
                        shardSet: params.getShardSet(),
                        indexSpec: oldShardKey,
                    }),
                );
            }
        }
    }
}

export {ShardingCommandGenerator};
