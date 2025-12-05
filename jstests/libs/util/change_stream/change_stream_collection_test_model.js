/**
 * Finite state machine for change stream configuration testing.
 * Manages state transitions for database and collection operations.
 */
import {State} from "jstests/libs/util/change_stream/change_stream_state.js";
import {Action} from "jstests/libs/util/change_stream/change_stream_action.js";

class CollectionTestModel {
    constructor() {
        this.states = new Set();
        this.transitions = new Map(); // state -> Map<action, toState>
        this.startState = null;
        this._initializeStates();
        this._configureTransitions();
    }

    setStartState(state) {
        assert(this.states.has(state), `State ${state} does not exist in the state machine`);
        this.startState = state;
        return this;
    }

    getStartState() {
        return this.startState;
    }

    collectionStateToActionsMap(state) {
        assert(this.states.has(state), `State ${state} does not exist in the state machine`);
        return this.transitions.get(state);
    }

    _initializeStates() {
        const states = [
            State.DATABASE_ABSENT,
            State.DATABASE_PRESENT_COLLECTION_ABSENT,
            State.COLLECTION_PRESENT_SHARDED,
            State.COLLECTION_PRESENT_UNSPLITTABLE,
            State.COLLECTION_PRESENT_UNTRACKED,
        ];

        states.forEach((state) => this.initializeState(state));
    }

    _configureTransitions() {
        // ===== DATABASE_ABSENT state transitions =====
        this.setActions(State.DATABASE_ABSENT, [
            [Action.INSERT_DOC, State.COLLECTION_PRESENT_UNTRACKED],
            [Action.CREATE_DATABASE, State.DATABASE_PRESENT_COLLECTION_ABSENT],
        ]);

        // ===== DATABASE_PRESENT_COLLECTION_ABSENT state transitions =====
        this.setActions(State.DATABASE_PRESENT_COLLECTION_ABSENT, [
            [Action.INSERT_DOC, State.COLLECTION_PRESENT_UNTRACKED],
            [Action.CREATE_SHARDED_COLLECTION, State.COLLECTION_PRESENT_SHARDED],
            [Action.CREATE_UNSPLITTABLE_COLLECTION, State.COLLECTION_PRESENT_UNSPLITTABLE],
            [Action.CREATE_UNTRACKED_COLLECTION, State.COLLECTION_PRESENT_UNTRACKED],
            [Action.DROP_DATABASE, State.DATABASE_ABSENT],
            [Action.MOVE_PRIMARY, State.DATABASE_PRESENT_COLLECTION_ABSENT],
        ]);

        // ===== COLLECTION_PRESENT_SHARDED state transitions =====
        this.setActions(State.COLLECTION_PRESENT_SHARDED, [
            [Action.INSERT_DOC, State.COLLECTION_PRESENT_SHARDED],
            [Action.DROP_COLLECTION, State.DATABASE_PRESENT_COLLECTION_ABSENT],
            [Action.DROP_DATABASE, State.DATABASE_ABSENT],
            [Action.RENAME_TO_NON_EXISTENT_SAME_DB, State.DATABASE_PRESENT_COLLECTION_ABSENT],
            [Action.RENAME_TO_EXISTENT_SAME_DB, State.DATABASE_PRESENT_COLLECTION_ABSENT],
            // Cross-database renames not supported for sharded collections.
            [Action.UNSHARD_COLLECTION, State.COLLECTION_PRESENT_UNSPLITTABLE],
            [Action.RESHARD_COLLECTION, State.COLLECTION_PRESENT_SHARDED],
            [Action.MOVE_PRIMARY, State.COLLECTION_PRESENT_SHARDED],
            // MOVE_COLLECTION only works on unsharded collections.
            [Action.MOVE_CHUNK, State.COLLECTION_PRESENT_SHARDED],
        ]);

        // ===== COLLECTION_PRESENT_UNSPLITTABLE state transitions =====
        this.setActions(State.COLLECTION_PRESENT_UNSPLITTABLE, [
            [Action.INSERT_DOC, State.COLLECTION_PRESENT_UNSPLITTABLE],
            [Action.DROP_COLLECTION, State.DATABASE_PRESENT_COLLECTION_ABSENT],
            [Action.DROP_DATABASE, State.DATABASE_ABSENT],
            [Action.RENAME_TO_NON_EXISTENT_SAME_DB, State.DATABASE_PRESENT_COLLECTION_ABSENT],
            [Action.RENAME_TO_EXISTENT_SAME_DB, State.DATABASE_PRESENT_COLLECTION_ABSENT],
            // Cross-database renames not supported for tracked collections.
            [Action.SHARD_COLLECTION, State.COLLECTION_PRESENT_SHARDED],
            [Action.MOVE_PRIMARY, State.COLLECTION_PRESENT_UNSPLITTABLE],
            [Action.MOVE_COLLECTION, State.COLLECTION_PRESENT_UNSPLITTABLE],
        ]);

        // ===== COLLECTION_PRESENT_UNTRACKED state transitions =====
        this.setActions(State.COLLECTION_PRESENT_UNTRACKED, [
            [Action.INSERT_DOC, State.COLLECTION_PRESENT_UNTRACKED],
            [Action.DROP_COLLECTION, State.DATABASE_PRESENT_COLLECTION_ABSENT],
            [Action.DROP_DATABASE, State.DATABASE_ABSENT],
            [Action.RENAME_TO_NON_EXISTENT_SAME_DB, State.DATABASE_PRESENT_COLLECTION_ABSENT],
            [Action.RENAME_TO_EXISTENT_SAME_DB, State.DATABASE_PRESENT_COLLECTION_ABSENT],
            // Cross-database renames require source and target on same shard.
            [Action.SHARD_COLLECTION, State.COLLECTION_PRESENT_SHARDED],
            [Action.MOVE_PRIMARY, State.COLLECTION_PRESENT_UNTRACKED],
            [Action.MOVE_COLLECTION, State.COLLECTION_PRESENT_UNTRACKED],
        ]);
    }

    initializeState(state) {
        if (!this.states.has(state)) {
            this.states.add(state);
            this.transitions.set(state, new Map());
        }
        return this;
    }

    setAction(fromState, action, toState) {
        assert(this.states.has(fromState), `State ${fromState} does not exist in the state machine`);
        assert(this.states.has(toState), `State ${toState} does not exist in the state machine`);
        assert(Action.getName(action), `Invalid action ID: ${action}`);

        const fromStateTransitions = this.transitions.get(fromState);
        fromStateTransitions.set(action, toState);

        return this;
    }

    /**
     * Set multiple transitions from a state at once.
     * @param {number} fromState - The source state
     * @param {Array} transitions - Array of [action, toState] pairs
     */
    setActions(fromState, transitions) {
        for (const [action, toState] of transitions) {
            this.setAction(fromState, action, toState);
        }
        return this;
    }
}

export {CollectionTestModel};
