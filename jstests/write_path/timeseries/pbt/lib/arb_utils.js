/*
 * Shared arbitrary utilities for property-based timeseries tests.
 */

import {fc} from "jstests/third_party/fast_check/fc-4.6.0.js";

/**
 * Produce uniformly distributed real numbers in [min, max].
 *
 * fc.double skews toward extreme values (±0, ±Infinity boundaries, denormals).
 * This helper uses fc.integer under the hood, which is truly uniform, then
 * scales into the floating-point range.
 *
 * @param {number} min   minimum value (inclusive)
 * @param {number} max   maximum value (inclusive)
 * @param {number} [resolution=1000]  number of steps per integer unit
 * @returns {fc.Arbitrary<number>}
 */
export const normalDistRealArb = (min, max, resolution = 1000.0) => {
    return fc
        .integer({min: min * resolution, max: max * resolution})
        .map((x) => Math.min(Math.max(x / resolution, min), max));
};
