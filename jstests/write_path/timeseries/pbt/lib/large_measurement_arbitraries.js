/*
 * Arbitraries for generating Large Measurements
 */

import {fc} from "jstests/third_party/fast_check/fc-4.6.0.js";

// Timeseries measurement sizes are only restricted in the write path by the total size of the buckets document.
const measurementMaxSizeLimit = 4 * 1024 * 1024;

/** A factory function that produces an arbitrary which creates large string sizes. This is intended to exercise BSON size limits or target specific measurement sizes.
 *
 * @param {Number} maximumSize the maximum size of string fields to create
 * @param {Number} reps repetitions of the string to use, this helps speed up arbitrary generation as a smaller subset can be reproduced rather than creating a very large arbitrary in one function call.
 * @returns {fc.Arbitrary<string>}
 */
export const makeLargeStringArb = (maximumSize = measurementMaxSizeLimit, reps = 1000) => {
    const minLength = 0;
    const maxLength = Math.max(1, Math.trunc(maximumSize / reps));
    return fc
        .string({minLength, maxLength, unit: "grapheme-ascii", size: "max"})
        .map((baseStr) => baseStr.repeat(reps));
};
