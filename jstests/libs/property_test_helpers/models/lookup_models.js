/*
 * $lookup models for our core property tests.
 */

import {assignableFieldArb, nonEmptyFieldArb} from "jstests/libs/property_test_helpers/models/basic_models.js";
import {fc} from "jstests/third_party/fast_check/fc-3.1.0.js";

export function getEqLookupArb(from) {
    return fc
        .record({
            localField: nonEmptyFieldArb,
            foreignField: nonEmptyFieldArb,
            as: assignableFieldArb,
        })
        .map(({localField, foreignField, as}) => {
            return {$lookup: {from, localField, foreignField, as}};
        });
}

export function getEqLookupUnwindArb(fromArb) {
    return fc
        .record({
            from: fromArb,
            localField: nonEmptyFieldArb,
            foreignField: nonEmptyFieldArb,
            as: assignableFieldArb,
            preserveNullAndEmptyArrays: fc.boolean(),
        })
        .map(({from, localField, foreignField, as, preserveNullAndEmptyArrays}) => {
            // The foreign side matches may appear out-of-order between control and experiment, so $unwind them as a workaround for result set comparison.
            const unwindSpec = {path: "$" + as};
            if (preserveNullAndEmptyArrays) {
                unwindSpec.preserveNullAndEmptyArrays = true;
            }
            return [{$lookup: {from, localField, foreignField, as}}, {$unwind: unwindSpec}];
        });
}
