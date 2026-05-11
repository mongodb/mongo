/**
 * Finite state machine for change stream configuration testing.
 * Manages state transitions for database and collection operations.
 *
 * ========================================================================================
 * NOTES
 * ========================================================================================
 *
 * ── createIndexes / dropIndexes ──────────────────────────────────────────────────
 *    These events are excluded at the reader level (kExcludedOperationTypes in
 *    change_stream_sharding_utils.js, applied as a $nin pipeline filter) and
 *    omitted from expected events (getChangeEvents() returns [] for
 *    CreateIndexCommand / DropIndexCommand). Their count is per-shard and
 *    non-deterministic because index DDL events are emitted on each shard
 *    independently.
 * ========================================================================================
 */
import {State} from "jstests/libs/util/change_stream/change_stream_state.js";
import {Action} from "jstests/libs/util/change_stream/change_stream_action.js";

class CollectionTestModel {
    /**
     * @param {number} startState - One of State.DATABASE_ABSENT or
     *   State.DATABASE_PRESENT_COLLECTION_ABSENT. The graph is configured for the
     *   chosen start state so the FSM generator can exercise all reachable actions.
     */
    constructor(startState) {
        assert(
            startState === State.DATABASE_ABSENT || startState === State.DATABASE_PRESENT_COLLECTION_ABSENT,
            `Unsupported start state: ${startState}`,
        );
        this.states = new Set();
        this.transitions = new Map();
        this.startState = startState;

        if (startState === State.DATABASE_ABSENT) {
            this._configureDbAbsent();
        } else {
            this._configureDbPresentNoDrops();
        }
    }

    getStartState() {
        return this.startState;
    }

    collectionStateToActionsMap(state) {
        assert(this.states.has(state), `State ${state} does not exist in the state machine`);
        return this.transitions.get(state);
    }

    /**
     * "db absent" mode.
     *
     * The generator starts at DATABASE_ABSENT. DROP_COLLECTION and RENAME provide
     * return paths from collection-present states to DB_PRESENT_COLL_ABSENT,
     * allowing the generator to cycle through
     * all create and collection-type transitions.
     *
     * DROP_DATABASE is omitted from DB_PRESENT_COLL_ABSENT but available from
     * collection-present states (returning to DATABASE_ABSENT for full lifecycle testing).
     */
    _configureDbAbsent() {
        this._initializeState(State.DATABASE_ABSENT);
        this._initializeCollectionStates();

        this._setActions(State.DATABASE_ABSENT, [
            [Action.INSERT_DOC, State.COLLECTION_PRESENT_UNTRACKED],
            [Action.CREATE_DATABASE, State.DATABASE_PRESENT_COLLECTION_ABSENT],
        ]);

        this._configureDbPresentCollectionAbsent();
        this._configureCollectionPresentStates(/* includeDropDatabase */ true, /* includeCrossDbRename */ true);
    }

    /**
     * "db present, no drops" mode.
     *
     * The generator starts at DATABASE_PRESENT_COLLECTION_ABSENT. DATABASE_ABSENT
     * and DROP_DATABASE are excluded entirely. DROP_COLLECTION and same-DB RENAME provide
     * return paths from collection-present states to DB_PRESENT_COLL_ABSENT.
     * Cross-DB rename is excluded: coverage is provided by "db absent" mode.
     */
    _configureDbPresentNoDrops() {
        this._initializeCollectionStates();

        this._configureDbPresentCollectionAbsent();
        this._configureCollectionPresentStates(/* includeDropDatabase */ false, /* includeCrossDbRename */ false);
    }

    _initializeCollectionStates() {
        this._initializeState(State.DATABASE_PRESENT_COLLECTION_ABSENT);
        this._initializeState(State.COLLECTION_PRESENT_SHARDED_RANGE);
        this._initializeState(State.COLLECTION_PRESENT_SHARDED_HASHED);
        this._initializeState(State.COLLECTION_PRESENT_UNSPLITTABLE);
        this._initializeState(State.COLLECTION_PRESENT_UNTRACKED);
    }

    _configureDbPresentCollectionAbsent() {
        this._setActions(State.DATABASE_PRESENT_COLLECTION_ABSENT, [
            [Action.INSERT_DOC, State.COLLECTION_PRESENT_UNTRACKED],
            [Action.CREATE_SHARDED_COLLECTION_RANGE, State.COLLECTION_PRESENT_SHARDED_RANGE],
            [Action.CREATE_SHARDED_COLLECTION_HASHED, State.COLLECTION_PRESENT_SHARDED_HASHED],
            [Action.CREATE_UNSPLITTABLE_COLLECTION, State.COLLECTION_PRESENT_UNSPLITTABLE],
            [Action.CREATE_UNTRACKED_COLLECTION, State.COLLECTION_PRESENT_UNTRACKED],
            [Action.MOVE_PRIMARY, State.DATABASE_PRESENT_COLLECTION_ABSENT],
        ]);
    }

    /**
     * Collection-present state transitions shared by both modes.
     * @param {boolean} includeDropDatabase - Whether to include DROP_DATABASE transitions.
     *   True for "db absent" mode (DATABASE_ABSENT exists), false for "no drops" mode.
     * @param {boolean} includeCrossDbRename - Whether to include cross-DB rename transitions.
     *   Only applies to COLLECTION_PRESENT_UNSPLITTABLE (sharded collections cannot cross-DB
     *   rename; untracked coverage is subsumed by the moveCollection → rename path on unsplittable).
     */
    _configureCollectionPresentStates(includeDropDatabase, includeCrossDbRename) {
        // ===== COLLECTION_PRESENT_SHARDED_RANGE =====
        this._setActions(State.COLLECTION_PRESENT_SHARDED_RANGE, [
            [Action.INSERT_DOC, State.COLLECTION_PRESENT_SHARDED_RANGE],
            [Action.DROP_COLLECTION, State.DATABASE_PRESENT_COLLECTION_ABSENT],
            ...(includeDropDatabase ? [[Action.DROP_DATABASE, State.DATABASE_ABSENT]] : []),
            [Action.RENAME_TO_NON_EXISTENT_SAME_DB, State.DATABASE_PRESENT_COLLECTION_ABSENT],
            [Action.RENAME_TO_EXISTENT_SAME_DB, State.DATABASE_PRESENT_COLLECTION_ABSENT],
            [Action.UNSHARD_COLLECTION, State.COLLECTION_PRESENT_UNSPLITTABLE],
            [Action.RESHARD_COLLECTION_TO_HASHED, State.COLLECTION_PRESENT_SHARDED_HASHED],
            [Action.MOVE_PRIMARY, State.COLLECTION_PRESENT_SHARDED_RANGE],
            [Action.MOVE_CHUNK, State.COLLECTION_PRESENT_SHARDED_RANGE],
        ]);

        // ===== COLLECTION_PRESENT_SHARDED_HASHED =====
        this._setActions(State.COLLECTION_PRESENT_SHARDED_HASHED, [
            [Action.INSERT_DOC, State.COLLECTION_PRESENT_SHARDED_HASHED],
            [Action.DROP_COLLECTION, State.DATABASE_PRESENT_COLLECTION_ABSENT],
            ...(includeDropDatabase ? [[Action.DROP_DATABASE, State.DATABASE_ABSENT]] : []),
            [Action.RENAME_TO_NON_EXISTENT_SAME_DB, State.DATABASE_PRESENT_COLLECTION_ABSENT],
            [Action.RENAME_TO_EXISTENT_SAME_DB, State.DATABASE_PRESENT_COLLECTION_ABSENT],
            [Action.UNSHARD_COLLECTION, State.COLLECTION_PRESENT_UNSPLITTABLE],
            [Action.RESHARD_COLLECTION_TO_RANGE, State.COLLECTION_PRESENT_SHARDED_RANGE],
            [Action.MOVE_PRIMARY, State.COLLECTION_PRESENT_SHARDED_HASHED],
            [Action.MOVE_CHUNK, State.COLLECTION_PRESENT_SHARDED_HASHED],
        ]);

        // Cross-DB rename is rejected by the server for sharded sources, so it only
        // appears in the unsplittable/untracked states below.

        // ===== COLLECTION_PRESENT_UNSPLITTABLE =====
        this._setActions(State.COLLECTION_PRESENT_UNSPLITTABLE, [
            [Action.INSERT_DOC, State.COLLECTION_PRESENT_UNSPLITTABLE],
            [Action.DROP_COLLECTION, State.DATABASE_PRESENT_COLLECTION_ABSENT],
            ...(includeDropDatabase ? [[Action.DROP_DATABASE, State.DATABASE_ABSENT]] : []),
            [Action.RENAME_TO_NON_EXISTENT_SAME_DB, State.DATABASE_PRESENT_COLLECTION_ABSENT],
            [Action.RENAME_TO_EXISTENT_SAME_DB, State.DATABASE_PRESENT_COLLECTION_ABSENT],
            ...(includeCrossDbRename
                ? [
                      [Action.RENAME_TO_NON_EXISTENT_DIFFERENT_DB, State.DATABASE_PRESENT_COLLECTION_ABSENT],
                      [Action.RENAME_TO_EXISTENT_DIFFERENT_DB, State.DATABASE_PRESENT_COLLECTION_ABSENT],
                  ]
                : []),
            [Action.SHARD_COLLECTION_RANGE, State.COLLECTION_PRESENT_SHARDED_RANGE],
            [Action.SHARD_COLLECTION_HASHED, State.COLLECTION_PRESENT_SHARDED_HASHED],
            [Action.MOVE_PRIMARY, State.COLLECTION_PRESENT_UNSPLITTABLE],
            [Action.MOVE_COLLECTION, State.COLLECTION_PRESENT_UNSPLITTABLE],
        ]);

        // ===== COLLECTION_PRESENT_UNTRACKED =====
        // TODO SERVER-126280: Cross-DB rename is excluded for untracked: coverage is provided by
        // COLLECTION_PRESENT_UNSPLITTABLE, which exercises the moveCollection → rename path.
        this._setActions(State.COLLECTION_PRESENT_UNTRACKED, [
            [Action.INSERT_DOC, State.COLLECTION_PRESENT_UNTRACKED],
            [Action.DROP_COLLECTION, State.DATABASE_PRESENT_COLLECTION_ABSENT],
            ...(includeDropDatabase ? [[Action.DROP_DATABASE, State.DATABASE_ABSENT]] : []),
            [Action.RENAME_TO_NON_EXISTENT_SAME_DB, State.DATABASE_PRESENT_COLLECTION_ABSENT],
            [Action.RENAME_TO_EXISTENT_SAME_DB, State.DATABASE_PRESENT_COLLECTION_ABSENT],
            [Action.SHARD_COLLECTION_RANGE, State.COLLECTION_PRESENT_SHARDED_RANGE],
            [Action.SHARD_COLLECTION_HASHED, State.COLLECTION_PRESENT_SHARDED_HASHED],
            [Action.MOVE_PRIMARY, State.COLLECTION_PRESENT_UNTRACKED],
            [Action.MOVE_COLLECTION, State.COLLECTION_PRESENT_UNTRACKED],
        ]);
    }

    _initializeState(state) {
        this.states.add(state);
        this.transitions.set(state, new Map());
    }

    _setActions(fromState, transitions) {
        for (const [action, toState] of transitions) {
            assert(this.states.has(fromState), `State ${fromState} does not exist`);
            assert(this.states.has(toState), `State ${toState} does not exist`);
            this.transitions.get(fromState).set(action, toState);
        }
    }
}

export {CollectionTestModel};
