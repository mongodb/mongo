/**
 * Generate the minimum number of iterations to expect to test all of the states in
 * a fsm workload.
 *
 * The calculation uses the coupon collector's problem: ceil(n * H_n), where H_n is the nth
 * harmonic number, to determine the expected number of iterations.
 *
 * This library assumes that all states have a uniform probability of transitioning to the next
 * state.
 *
 * @param {Object} states - The states object from an FSM workload.
 * @param {number} concurrent_iterations - Multiplier to control how many times each state is run
 *        in a single thread. Increase this value to have each thread execute multiple instances of
 *        a state in parallel, which is useful for workloads with many operation types. The default
 *        value of 2 is a safety factor to ensure that we cover all states.
 * @returns {number} The minimum number of iterations.
 */
export function minimumIterations(states, concurrent_iterations = 2) {
    const stateNames = Object.keys(states).filter((stateName) => stateName !== "init");
    const n = stateNames.length;
    let harmonic = 0;
    for (let i = 1; i <= n; i++) {
        harmonic += 1 / i;
    }
    return Math.ceil(n * harmonic) * concurrent_iterations;
}
