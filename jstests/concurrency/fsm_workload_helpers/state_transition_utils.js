
/*
 * Generate uniform distribution transitions matrix for the given states
 *
 * All states can be transitioned from all the others with equal probability.
 *
 * e.g.
 * {
 * 	state_1: {state_1: 0.5, state_2: 0.5};
 * 	state_2: {state_1: 0.5, state_2: 0.5};
 * }
 */

function uniformDistTransitions(states) {
    let stateNames = Object.keys(states);
    let reachableStateNames = stateNames.filter(stateName => stateName != 'init');
    let prob = 1 / reachableStateNames.length;
    let transitions = {};
    reachableStateNames.forEach(stateName => {
        transitions[stateName] = prob;
    });

    let allTransitions = {};
    stateNames.forEach(stateName => {
        allTransitions[stateName] = transitions;
    });

    return allTransitions;
}
