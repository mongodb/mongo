/*
 * $lookup models for our core property tests.
 */

import {assignableFieldArb, fieldArb} from "jstests/libs/property_test_helpers/models/basic_models.js";
import {fc} from "jstests/third_party/fast_check/fc-3.1.0.js";

export function getEqLookupUnwindArb(from) {
    return fc
        .record({
            localField: fieldArb,
            foreignField: fieldArb,
            as: assignableFieldArb,
        })
        .map(({localField, foreignField, as}) => {
            // The foreign side matches may appear out-of-order between control and experiment, so $unwind them as a workaround for result set comparison.
            return [{$lookup: {from, localField, foreignField, as}}, {$unwind: {path: "$" + as}}];
        });
}
