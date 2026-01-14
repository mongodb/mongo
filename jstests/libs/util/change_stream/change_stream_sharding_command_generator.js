/**
 * Generates commands that explore all states and transitions in a test model.
 * Uses the jstest shell's Random implementation for reproducibility.
 * Requires an explicit seed to ensure deterministic behavior.
 */
import {Action} from "jstests/libs/util/change_stream/change_stream_action.js";
import {State} from "jstests/libs/util/change_stream/change_stream_state.js";
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

class ShardingCommandGenerator {
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

    constructor(seed) {
        assert(seed !== null && seed !== undefined, "Seed must be explicitly provided to ShardingCommandGenerator");
        this.seed = seed;
    }

    getSeed() {
        return this.seed;
    }

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

        // Commands that need the target shard key as a separate parameter.
        if (CommandClass === ShardCollectionCommand || CommandClass === ReshardCollectionCommand) {
            assert(targetShardKey, `${CommandClass.name} requires targetShardKey`);
            return new CommandClass(
                params.getDbName(),
                params.getCollName(),
                params.getShardSet(),
                {...collectionCtx},
                targetShardKey,
            );
        }

        return new CommandClass(params.getDbName(), params.getCollName(), params.getShardSet(), {...collectionCtx});
    }

    /**
     * Generates a sequence of commands that covers all transitions in the state machine.
     * Step 1: Precompute shortest paths between all pairs of states.
     * Step 2: Randomly pick unvisited actions; navigate via precomputed paths when needed.
     * @param {CollectionTestModel} testModel - The state machine model.
     * @param {ShardingCommandGeneratorParams} params - The generator parameters.
     * @returns {Array<Command>} Array of commands.
     */
    generateCommands(testModel, params) {
        // Re-seed to ensure reproducibility since Random is a global singleton.
        Random.srand(this.seed);

        const startState = testModel.getStartState();
        assert(startState !== null, "Start state must be set before generating commands");

        // Step 1: For each state, compute shortest paths to all other states using BFS.
        const shortestPaths = this._precomputeAllShortestPaths(testModel);

        // Step 2: For each state, record unvisited actions.
        const unvisitedActions = this._initializeUnvisitedActions(testModel);

        // Step 3: Select starting state.
        let currentState = startState;
        let collectionCtx = this._initialCollectionCtxForState(startState);
        const commands = [];

        // Step 4: While there are unvisited actions.
        while (unvisitedActions.size > 0) {
            // 4a: For each unvisited self-loop action (state → state), visit and mark as visited.
            let selfLoopAction = this._getRandomUnvisitedSelfLoop(testModel, currentState, unvisitedActions);
            while (selfLoopAction !== null) {
                this._appendAction(commands, selfLoopAction, params, collectionCtx);
                this._updateCollectionCtxForAction(selfLoopAction, collectionCtx);
                this._markActionAsVisited(unvisitedActions, currentState, selfLoopAction);
                selfLoopAction = this._getRandomUnvisitedSelfLoop(testModel, currentState, unvisitedActions);
            }

            // 4b: If exists unvisited non-self-loop action, visit it and move to target state.
            const action = this._getRandomUnvisitedNonSelfLoop(testModel, currentState, unvisitedActions);
            if (action !== null) {
                this._appendAction(commands, action.action, params, collectionCtx);
                this._updateCollectionCtxForAction(action.action, collectionCtx);
                this._markActionAsVisited(unvisitedActions, currentState, action.action);
                currentState = action.to;
            } else if (unvisitedActions.size > 0) {
                // 4c: Find nearest state with unvisited actions and navigate via shortest path.
                const targetState = this._findNearestStateWithUnvisitedActions(
                    currentState,
                    unvisitedActions,
                    shortestPaths,
                );
                assert(targetState !== null, "State machine has unreachable states with unvisited actions");

                const path = shortestPaths.get(currentState).get(targetState).path;
                for (const step of path) {
                    this._appendAction(commands, step.action, params, collectionCtx);
                    this._updateCollectionCtxForAction(step.action, collectionCtx);
                    this._markActionAsVisited(unvisitedActions, step.from, step.action);
                }
                currentState = targetState;
            }
        }
        return commands;
    }

    /**
     * Precompute shortest paths from all states to all other states.
     * @returns {Map} Map<fromState, Map<toState, {distance, path}>>.
     */
    _precomputeAllShortestPaths(testModel) {
        const allPaths = new Map();
        for (const state of testModel.states) {
            allPaths.set(state, this._computeShortestPathsFromState(testModel, state));
        }
        return allPaths;
    }

    /**
     * Compute shortest paths from sourceState to all other states using BFS.
     */
    _computeShortestPathsFromState(testModel, sourceState) {
        const paths = new Map();
        const queue = [{state: sourceState, distance: 0, path: []}];
        const visited = new Set([sourceState]);

        while (queue.length > 0) {
            const {state, distance, path} = queue.shift();

            paths.set(state, {distance, path: [...path]});

            const actionsMap = testModel.collectionStateToActionsMap(state);
            for (const action of actionsMap.keys()) {
                const nextState = actionsMap.get(action);

                if (nextState !== undefined && !visited.has(nextState)) {
                    visited.add(nextState);
                    queue.push({
                        state: nextState,
                        distance: distance + 1,
                        path: [...path, {from: state, action, to: nextState}],
                    });
                }
            }
        }
        return paths;
    }

    /**
     * Initialize tracking of unvisited actions for each state.
     * Only states with actions are added to the map.
     */
    _initializeUnvisitedActions(testModel) {
        const unvisitedActions = new Map();
        for (const state of testModel.states) {
            const actions = new Set(testModel.collectionStateToActionsMap(state).keys());
            if (actions.size > 0) {
                unvisitedActions.set(state, actions);
            }
        }
        return unvisitedActions;
    }

    /**
     * Base method to get a randomly selected unvisited action from the state.
     * @param {boolean} isSelfLoop - True to filter for self-loops, false for non-self-loops.
     * @returns {{action: number, to: number}|null} The action and target state, or null.
     */
    _getRandomUnvisitedAction(testModel, state, unvisitedActions, isSelfLoop) {
        const unvisitedActionsForState = unvisitedActions.get(state);
        if (!unvisitedActionsForState || unvisitedActionsForState.size === 0) return null;

        const actionsMap = testModel.collectionStateToActionsMap(state);
        const candidateActions = Array.from(unvisitedActionsForState).filter((a) =>
            isSelfLoop ? actionsMap.get(a) === state : actionsMap.get(a) !== state,
        );
        if (candidateActions.length === 0) return null;

        const selectedAction = candidateActions[Random.randInt(candidateActions.length)];
        const nextState = actionsMap.get(selectedAction);
        return {action: selectedAction, to: nextState};
    }

    /**
     * Get a randomly selected unvisited self-loop action (state → state).
     * @returns {number|null} The action, or null if no unvisited self-loops exist.
     */
    _getRandomUnvisitedSelfLoop(testModel, state, unvisitedActions) {
        const result = this._getRandomUnvisitedAction(testModel, state, unvisitedActions, true);
        return result ? result.action : null;
    }

    /**
     * Get a randomly selected unvisited non-self-loop action (state → otherState).
     * @returns {{action: number, to: number}|null} The action and target state, or null.
     */
    _getRandomUnvisitedNonSelfLoop(testModel, state, unvisitedActions) {
        return this._getRandomUnvisitedAction(testModel, state, unvisitedActions, false);
    }

    _markActionAsVisited(unvisitedActions, state, action) {
        const unvisitedActionsForState = unvisitedActions.get(state);
        if (!unvisitedActionsForState) return;
        unvisitedActionsForState.delete(action);
        if (unvisitedActionsForState.size === 0) {
            unvisitedActions.delete(state);
        }
    }

    /**
     * Find nearest state with unvisited actions using precomputed paths.
     * @param {number} currentState - The current state.
     * @param {Map} unvisitedActions - Map of states to unvisited actions.
     * @param {Map} shortestPaths - Precomputed shortest paths.
     * @returns {number|null} The nearest state with unvisited actions, or null.
     */
    _findNearestStateWithUnvisitedActions(currentState, unvisitedActions, shortestPaths) {
        const pathsFromCurrent = shortestPaths.get(currentState);

        let nearest = null;
        let minDistance = Infinity;

        for (const state of unvisitedActions.keys()) {
            if (pathsFromCurrent.has(state)) {
                const distance = pathsFromCurrent.get(state).distance;
                if (distance < minDistance) {
                    minDistance = distance;
                    nearest = state;
                }
            }
        }
        return nearest;
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
            isSharded: false,
        };
    }

    /**
     * Initialize collection context based on the starting state.
     * collectionCtx tracks: exists, nonEmpty, shardKeySpec, isSharded.
     * shardKeySpec represents the collection's CURRENT shard key.
     * Note: Even if starting from a collection-present state, we assume the collection
     * is empty (nonEmpty: false) since we don't know its actual contents.
     */
    _initialCollectionCtxForState(state) {
        const ctx = ShardingCommandGenerator._emptyCollectionCtx();

        switch (state) {
            case State.DATABASE_ABSENT:
                // Nothing exists.
                break;
            case State.DATABASE_PRESENT:
                // Database exists but no collection.
                break;
            case State.COLLECTION_PRESENT_UNTRACKED:
                ctx.exists = true;
                break;
            case State.COLLECTION_PRESENT_UNSPLITTABLE:
                ctx.exists = true;
                // Unsplittable collections are tracked but not sharded in the traditional sense.
                break;
            case State.COLLECTION_PRESENT_SHARDED_RANGE:
                ctx.exists = true;
                ctx.isSharded = true;
                ctx.shardKeySpec = getShardKeySpec(ShardingType.RANGE);
                break;
            case State.COLLECTION_PRESENT_SHARDED_HASHED:
                ctx.exists = true;
                ctx.isSharded = true;
                ctx.shardKeySpec = getShardKeySpec(ShardingType.HASHED);
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
    _updateCollectionCtxForAction(action, ctx) {
        switch (action) {
            case Action.INSERT_DOC:
                ctx.exists = true;
                ctx.nonEmpty = true;
                break;
            case Action.CREATE_SHARDED_COLLECTION_RANGE:
            case Action.SHARD_COLLECTION_RANGE:
                ctx.exists = true;
                ctx.shardKeySpec = getShardKeySpec(ShardingType.RANGE);
                ctx.isSharded = true;
                break;
            case Action.CREATE_SHARDED_COLLECTION_HASHED:
            case Action.SHARD_COLLECTION_HASHED:
                ctx.exists = true;
                ctx.shardKeySpec = getShardKeySpec(ShardingType.HASHED);
                ctx.isSharded = true;
                break;
            case Action.RESHARD_COLLECTION_TO_RANGE:
            case Action.RESHARD_COLLECTION_TO_HASHED: {
                // Reshard: old index is explicitly dropped in _appendAction, new index is explicitly created.
                assert(ctx.shardKeySpec, "Reshard requires existing shard key");
                const newShardKey =
                    action === Action.RESHARD_COLLECTION_TO_RANGE
                        ? getShardKeySpec(ShardingType.RANGE)
                        : getShardKeySpec(ShardingType.HASHED);
                ctx.shardKeySpec = newShardKey;
                ctx.isSharded = true;
                break;
            }
            case Action.CREATE_UNSPLITTABLE_COLLECTION:
            case Action.CREATE_UNTRACKED_COLLECTION:
                ctx.exists = true;
                ctx.nonEmpty = false;
                ctx.isSharded = false;
                ctx.shardKeySpec = null;
                break;
            case Action.UNSHARD_COLLECTION:
                // Collection becomes unsplittable (single-shard) but still exists.
                // Old shard key index is explicitly dropped in _appendAction.
                assert(ctx.shardKeySpec, "Unshard requires existing shard key");
                ctx.shardKeySpec = null;
                ctx.isSharded = false;
                break;
            case Action.DROP_COLLECTION:
            case Action.DROP_DATABASE:
            case Action.RENAME_TO_NON_EXISTENT_SAME_DB:
            case Action.RENAME_TO_EXISTENT_SAME_DB:
            case Action.RENAME_TO_NON_EXISTENT_DIFFERENT_DB:
            case Action.RENAME_TO_EXISTENT_DIFFERENT_DB:
                // Collection no longer exists (dropped or renamed away).
                Object.assign(ctx, ShardingCommandGenerator._emptyCollectionCtx());
                break;
            default:
                // Other actions do not modify collection context.
                break;
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
     * Append the command(s) for a given action.
     * Some actions require prerequisite commands (e.g., index creation before sharding).
     */
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
                action === Action.SHARD_COLLECTION_RANGE || action === Action.SHARD_COLLECTION_HASHED,
                `Unexpected action ${action} on non-existent collection - only SHARD_COLLECTION_* allowed`,
            );

            const shardCtx = {...ctx, exists: false};
            commands.push(
                new ShardCollectionCommand(
                    params.getDbName(),
                    params.getCollName(),
                    params.getShardSet(),
                    shardCtx,
                    targetShardKey,
                ),
            );
            // Update context - collection is now sharded
            ctx.exists = true;
            ctx.isSharded = true;
            ctx.shardKeySpec = targetShardKey;
            return; // Skip index creation and main command - already done
        }

        // Create shard key index if action requires it (collection must already exist).
        if (ShardingCommandGenerator.actionsRequiringIndex.has(action)) {
            commands.push(
                new CreateIndexCommand(
                    params.getDbName(),
                    params.getCollName(),
                    params.getShardSet(),
                    ctx,
                    targetShardKey,
                ),
            );
        }
        // NOTE: We intentionally do NOT drop indexes before dropping collections.
        // While MongoDB silently removes indexes when dropping a collection (no 'dropIndexes' events),
        // we already get 'dropIndexes' coverage from the post-reshard/unshard cleanup (Step 5).
        // Explicitly dropping here would be redundant and add unnecessary complexity.
        // Drop collection before dropping database (simplifies change event matching).
        if (action === Action.DROP_DATABASE && collectionCtx.exists) {
            // NOTE: We do NOT drop indexes here - see comment above about dropIndexes coverage.
            // Pass a COPY to DropCollectionCommand so it sees exists:true.
            // We'll set ctx.exists = false for the subsequent DropDatabaseCommand.
            commands.push(
                new DropCollectionCommand(params.getDbName(), params.getCollName(), params.getShardSet(), {...ctx}),
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
                action === Action.UNSHARD_COLLECTION || bsonWoCompare(oldShardKey, targetShardKey) !== 0;
            if (shouldDropIndex && oldShardKey) {
                // Update context for DropIndexCommand's event count calculation:
                // - After reshard: shardKeySpec = new shard key (collection still sharded)
                // - After unshard: shardKeySpec = null (collection is now untracked, no shard key)
                const postActionCtx = {...ctx, shardKeySpec: targetShardKey};
                commands.push(
                    new DropIndexCommand(
                        params.getDbName(),
                        params.getCollName(),
                        params.getShardSet(),
                        postActionCtx,
                        oldShardKey,
                    ),
                );
            }
        }
    }
}

export {ShardingCommandGenerator};
