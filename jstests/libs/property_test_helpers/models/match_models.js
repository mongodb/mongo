/*
 * Fast-check models for $match.
 */
import {
    fieldArb,
    leafParameterArb
} from "jstests/libs/property_test_helpers/models/basic_models.js";
import {oneof} from "jstests/libs/property_test_helpers/models/model_utils.js";
import {fc} from "jstests/third_party/fast_check/fc-3.1.0.js";

const comparisonTypeArb = fc.constantFrom('$eq', '$ne', '$lt', '$lte', '$gt', '$gte');

/*
 * In this file, `condition` refers to a comparison that could be made against a field, but does
 * not include the field itself. `predicate` refers to the field and the comparison together.
 *
 * For example {a: {$lt: 5}} is a predicate, while {$lt: 5} is a condition.
 *
 * This helps us clearly define what each arbitrary is modeling.
 */
const simpleConditionArb =
    fc.record({op: comparisonTypeArb, arg: leafParameterArb}).map(({op, arg}) => {
        return {[op]: arg};
    });
const inConditionArb = fc.record({$in: fc.array(leafParameterArb, {maxLength: 3})});
const ninConditionArb = fc.record({$nin: fc.array(leafParameterArb, {maxLength: 3})});
const existsConditionArb = fc.record({$exists: fc.boolean()});

/*
 * Arbitrary $match expression that may contain simple comparisons, nested comparisons, $elemMatch,
 * and $not.
 * $or, $nor and $in are only allowed if `allowOrs` is true.
 */
export function getMatchArb(allowOrs) {
    const compoundOpArb = allowOrs ? fc.constantFrom('$and', '$or', '$nor') : fc.constant('$and');

    // $in is a form of OR, and shouldn't be generated when `allowOrs`=false.
    const leafConditionArb = allowOrs
        ? oneof(simpleConditionArb, existsConditionArb, inConditionArb, ninConditionArb)
        : oneof(simpleConditionArb, existsConditionArb);
    const matchPredicateArb =
        fc.letrec(
              tie => ({
                  // The expression under an $elemMatch can specify a field to compare to, or could
                  // only be a condition:
                  // {$elemMatch: {$gt: 5}}  # Look for an array element greater than 5
                  // {$elemMatch: {a: {$gt: 5}}}  # Look for an object in the array with field a > 5
                  // They must be the same type, all conditions or all predicates.
                  elemMatch: oneof(fc.array(tie('condition'), {minLength: 1, maxLength: 3}),
                                   fc.array(tie('predicate'), {minLength: 1, maxLength: 3}))
                                 .map(children => {
                                     const joinedPredicates = Object.assign({}, ...children);
                                     return {$elemMatch: joinedPredicates};
                                 }),
                  not: fc.record({$not: tie('condition')}),

                  // TODO SERVER-101007
                  // TODO SERVER-101260
                  // TODO SERVER-101795
                  // After these tickets are complete, re-enable $elemMatch.
                  condition: oneof(leafConditionArb,
                                   // tie('elemMatch'),
                                   tie('not')),

                  // $and/$or/$nor with child predicates.
                  compoundPredicate: fc.record({
                                           op: compoundOpArb,
                                           childPredicates: fc.array(tie('predicate'),
                                                                     {minLength: 1, maxLength: 3})
                                       }).map(({op, childPredicates}) => {
                      return {[op]: childPredicates};
                  }),
                  simplePredicate:
                      fc.record({field: fieldArb, expr: tie('condition')}).map(({field, expr}) => {
                          return {[field]: expr};
                      }),
                  predicate: fc.oneof({withCrossShrink: true, maxDepth: 5},
                                      tie('simplePredicate'),
                                      tie('compoundPredicate'))
              }))
            .predicate;
    return fc.record({$match: matchPredicateArb});
}