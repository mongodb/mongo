/**
 * Generates commands that explore all states and transitions in a test model.
 * Uses the jstest shell's Random implementation for reproducibility.
 * Requires an explicit seed to ensure deterministic behavior.
 */
class ShardingCommandGenerator {
    constructor(seed) {
        assert(seed !== null && seed !== undefined, "Seed must be explicitly provided to ShardingCommandGenerator");
        this.seed = seed;
    }

    getSeed() {
        return this.seed;
    }

    /**
     * Generates a sequence of commands that covers all transitions in the state machine.
     * Step 1: Precompute shortest paths between all pairs of states.
     * Step 2: Randomly pick unvisited actions; navigate via precomputed paths when needed.
     * @param {CollectionTestModel} testModel - The state machine model.
     * @returns {Array} Array of {from, action, to} transitions.
     */
    generateCommands(testModel) {
        // Re-seed to ensure reproducibility since Random is a global singleton
        Random.srand(this.seed);

        const startState = testModel.getStartState();
        assert(startState !== null, "Start state must be set on the test model before generating commands");

        // Step 1: For each state, compute shortest paths to all other states using BFS.
        const shortestPaths = this._precomputeAllShortestPaths(testModel);

        // Step 2: For each state, record unvisited actions.
        const unvisitedActions = this._initializeUnvisitedActions(testModel);

        // Step 3: Select starting state.
        let currentState = startState;
        const commands = [];

        // Step 4: While there are unvisited actions.
        while (unvisitedActions.size > 0) {
            // 4a: For each unvisited self-loop action (state → state), visit and mark as visited.
            let selfLoopAction = this._getRandomUnvisitedSelfLoop(testModel, currentState, unvisitedActions);
            while (selfLoopAction !== null) {
                commands.push(this._createTransition(currentState, selfLoopAction, currentState));
                this._markActionAsVisited(unvisitedActions, currentState, selfLoopAction);
                selfLoopAction = this._getRandomUnvisitedSelfLoop(testModel, currentState, unvisitedActions);
            }

            // 4b: If exists unvisited non-self-loop action, visit it and move to target state.
            const action = this._getRandomUnvisitedNonSelfLoop(testModel, currentState, unvisitedActions);
            if (action !== null) {
                commands.push(this._createTransition(currentState, action.action, action.to));
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
                    commands.push(this._createTransition(step.from, step.action, step.to));
                    this._markActionAsVisited(unvisitedActions, step.from, step.action);
                }
                currentState = targetState;
            }
        }

        return commands;
    }

    _createTransition(from, action, to) {
        return {from, action, to};
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
}

export {ShardingCommandGenerator};
