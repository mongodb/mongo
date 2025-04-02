/*
 * Helper functions for our core property test models.
 */
import {fc} from "jstests/third_party/fast_check/fc-3.1.0.js";

// fc.oneof with better shrinking enabled. Should be used by default.
// If additional options need to be passed, use `fc.oneof`.
export const oneof = function(...arbs) {
    return fc.oneof({withCrossShrink: true}, ...arbs);
};

// Creates an arbitrary that generates objects with one key/val, using the given key arbitrary and
// value arbitrary.
export const singleKeyObjArb = function(keyArb, valueArb) {
    return fc.record({key: keyArb, value: valueArb}).map(({key, value}) => ({[key]: value}));
};
