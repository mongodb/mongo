/*
 * $group models for our core property tests.
 *
 * Note that $avg cannot be supported because it can cause floating point differences in results.
 */
import {
    assignableFieldArb,
    dollarFieldArb
} from "jstests/libs/property_test_helpers/models/basic_models.js";
import {oneof} from "jstests/libs/property_test_helpers/models/model_utils.js";
import {fc} from "jstests/third_party/fast_check/fc-3.1.0.js";

// These all model accumulated fields, which is the output field and the accumulator. An example
// is `a: {count: {}}` which can by used in a $group.
const countAccArb = assignableFieldArb.map(out => {
    return {[out]: {$count: {}}};
});

// $sum. Example is `a: {$sum: '$b'}`.
const sumAccArb =
    fc.record({input: dollarFieldArb, output: assignableFieldArb}).map(({input, output}) => {
        return {[output]: {$sum: input}};
    });

// Examples are `a: {$min: '$b'}` and `a: {$min: {input: b, n: 2}}`.
const minMaxAccArb = fc.record({
                           acc: fc.constantFrom('$min', '$max'),
                           input: dollarFieldArb,
                           output: assignableFieldArb,
                           n: fc.option(fc.integer({min: 1, max: 3}))
                       }).map(({acc, input, output, n}) => {
    let accSpec;
    if (n) {
        // $min becomes $minN, $max becomes $maxN.
        const accN = acc + 'N';
        accSpec = {[accN]: {input, n}};
    } else {
        accSpec = {[acc]: input};
    }
    return {[output]: accSpec};
});

// The accumulators we support are $count, $sum, and $min/$max. Accumulators such as $top and
// $bottom run into issues with ties in the sort order. It's impossible to incorporate those into
// the core PBTs, so we'll have to write a dedicated standalone PBT for them.
const accumulatedFieldArb = oneof(countAccArb, sumAccArb, minMaxAccArb);

// A groupby key could be a single field `$a` or an object, `{a: '$b', c: '$d'}`
const objectGbKeyArb = fc.dictionary(assignableFieldArb, dollarFieldArb, {minKeys: 1, maxKeys: 3});
const groupByKeyArb = oneof(
    dollarFieldArb,
    // TODO SERVER-102229, re-enable object group keys once issue is fixed.
    // objectGbKeyArb
);

export const groupArb =
    fc.record({
          gbField: fc.option(groupByKeyArb),
          accumulatedFields: fc.array(accumulatedFieldArb, {minLength: 0, maxLength: 3})
      }).map(({gbField, accumulatedFields}) => {
        // Merge the groupby key and all accumulated fields.
        const groupSpec = Object.assign({_id: gbField}, ...accumulatedFields);
        return {$group: groupSpec};
    });
