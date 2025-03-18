/*
 * Helper functions for our core property test models.
 */
import {fc} from "jstests/third_party/fast_check/fc-3.1.0.js";

// fc.oneof with better shrinking enabled. Should be used by default.
// If additional options need to be passed, use `fc.oneof`.
export const oneof = function(...arbs) {
    return fc.oneof({withCrossShrink: true}, ...arbs);
};
