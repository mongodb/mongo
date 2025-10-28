import {fc} from "jstests/third_party/fast_check/fc-3.1.0.js";

/*
 * Fast-check models for generating collations.
 */
export const collationArb = fc
    .record(
        {
            "locale": fc.constantFrom("simple", "en", "en_US", "de", "fr", "ru", "zh"),
            "strength": fc.integer({min: 1, max: 5}),
            "caseLevel": fc.boolean(),
            "caseFirst": fc.constantFrom("upper", "lower", "off"),
            "numericOrdering": fc.boolean(),
            "alternate": fc.constantFrom("non-ignorable", "shifted"),
            "maxVariable": fc.constantFrom("punct", "space"),
            "backwards": fc.boolean(),
            "normalization": fc.boolean(),
        },
        {requiredKeys: ["locale"]},
    )
    .filter((collation) => {
        // Filter out invalid collations.

        // 'backwards' is invalid with 'strength' of 1
        if (collation.backwards && collation.strength === 1) {
            return false;
        }

        // 'caseFirst' is invalid if caseLevel is off and strength is 1 or 2.
        if (
            collation.caseFirst !== "off" &&
            !collation.caseLevel &&
            (collation.strength === 1 || collation.strength === 2)
        ) {
            return false;
        }
        return true;
    });
