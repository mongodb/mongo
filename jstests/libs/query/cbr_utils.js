/**
 * Utilities for writing tests for the cost-based ranker (CBR).
 */

// Round the given number to the nearest 0.1
function round(num) {
    return Math.round(num * 10) / 10;
}

// Compare two cardinality estimates with approximate equality. This is done so that our tests can
// be robust to floating point rounding differences but detect large changes in estimation
// indicating a real change in estimation behavior.
export function ceEqual(lhs, rhs) {
    return Math.abs(round(lhs) - round(rhs)) < 0.1;
}

function planEstimateTypeIs(plan, type) {
    return plan.estimatesMetadata.ceSource === type;
}

// Return true if the plan was estimated with a histogam estimation source. Return false otherwise.
export function planEstimatedWithHistogram(plan) {
    return planEstimateTypeIs(plan, "Histogram");
}

/**
 * Assert the given plan was not costed. This is used as an indicator to whether CBR used its
 * fallback to multiplanning for this plan.
 */
export function assertPlanNotCosted(plan) {
    assert(!plan.hasOwnProperty('costEstimate'), plan);
}
