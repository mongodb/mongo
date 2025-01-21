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
