import {fc} from "jstests/third_party/fast_check/fc-3.1.0.js";

/*
 * Fast-check models for generating collations.
 */
// TODO SERVER-111679: Support all documented collation-related fields.
export const collationArb = fc.record({
    "locale": fc.constantFrom("simple", "en", "en_US", "de", "fr", "ru", "zh"),
    "strength": fc.integer({min: 1, max: 5}),
});
