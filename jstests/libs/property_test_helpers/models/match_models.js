/*
 * Fast-check models for $match.
 */
import {
    fieldArb,
    leafParameterArb
} from "jstests/libs/property_test_helpers/models/basic_models.js";
import {oneof, singleKeyObjArb} from "jstests/libs/property_test_helpers/models/model_utils.js";
import {fc} from "jstests/third_party/fast_check/fc-3.1.0.js";

const simpleComparators = ['$eq', '$ne', '$lt', '$lte', '$gt', '$gte'];

function makeSimpleConditionArb(leafArb, allowedSimpleComparisons) {
    // The simplest conditions use comparators like $eq, $gt, $lte. The keys are comparators and the
    // values are the specified leaf arbitraries. `numConditions` specifies how many conditions to
    // have in the arbitrary.
    // For example with numConditions=2, we could generate `{$gt: 5, $lte: 10}`
    const makeSimpleConditionHelper = function(numConditions) {
        return fc.dictionary(fc.constantFrom(...allowedSimpleComparisons),
                             leafArb,
                             {minKeys: numConditions, maxKeys: numConditions});
    };
    // Weigh arbitraries with less conditions higher. An arbitrary with one condition is the most
    // common, with two is less common, three is rare.
    // Three conditions is likely to always be false or have one condition be redundant, like
    // `{$gt: 5, $lte 10, $gte: 6}`. But we include it for completeness.
    return oneof({arbitrary: makeSimpleConditionHelper(1 /* numConditions */), weight: 5},
                 {arbitrary: makeSimpleConditionHelper(2 /* numConditions */), weight: 3},
                 {arbitrary: makeSimpleConditionHelper(3 /* numConditions */), weight: 1});
}

/*
 * In this file, `condition` refers to a comparison that could be made against a field, but does
 * not include the field itself. `predicate` refers to the field and the comparison together.
 *
 * For example {a: {$lt: 5}} is a predicate, while {$lt: 5} is a condition.
 *
 * This helps us clearly define what each arbitrary is modeling.
 */
function getLeafConditionArb(
    {leafArb, allowedSimpleComparisons, allowedExistsArgs, allowIn, allowNin}) {
    const leafConditionArbs = [makeSimpleConditionArb(leafArb, allowedSimpleComparisons)];
    if (allowedExistsArgs.length > 0) {
        const existsConditionArb = fc.record({$exists: fc.constantFrom(...allowedExistsArgs)});
        leafConditionArbs.push(existsConditionArb);
    }
    if (allowIn) {
        const inConditionArb = fc.record({$in: fc.array(leafArb, {maxLength: 3})});
        leafConditionArbs.push(inConditionArb);
    }
    if (allowNin) {
        const ninConditionArb = fc.record({$nin: fc.array(leafArb, {maxLength: 3})});
        leafConditionArbs.push(ninConditionArb);
    }

    return oneof(...leafConditionArbs);
}

// A configurable predicate model. Provides fine-grained control of which operators are allowed in
// the predicate.
function getMatchPredicateSpec({
    // Specifies the arbitrary to place at the leaf of comparisons. For example, for
    // {a: {$eq: _}}
    // We could place a constant scalar, or a list of scalars (the default) to parameterize the
    // predicate shape, forming a `query family` (defined in the README)
    leafArb = leafParameterArb,
    maxDepth = 5,
    allowOrs = true,
    allowNors = true,
    allowNot = true,
    // Leaf comparison types, like $eq, $ne, $gt, etc.
    allowedSimpleComparisons = simpleComparators,
    allowIn = true,
    allowNin = true,
    // $exists is disallowed if this set is empty.
    allowedExistsArgs = [true, false]
} = {}) {
    const compoundOps = ['$and'];
    if (allowOrs) {
        compoundOps.push('$or');
    }
    if (allowNors) {
        compoundOps.push('$nor');
    }
    const leafConditionArb = getLeafConditionArb(
        {leafArb, allowedSimpleComparisons, allowedExistsArgs, allowIn, allowNin});

    // For recursive arbitraries, the `tie` function is how we refer to other arbitraries involved
    // in the recursion. We can't directly refer to them, since they're not created yet.
    return fc.letrec(tie => {
        const allowedConditions = [
            leafConditionArb,
            // TODO SERVER-101007
            // TODO SERVER-101260
            // TODO SERVER-101795
            // After these tickets are complete, re-enable $elemMatch.
            // tie('elemMatch')
        ];
        if (allowNot) {
            allowedConditions.push(tie('not'));
        }

        return {
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
            condition: oneof(...allowedConditions),

            // $and/$or/$nor with child predicates.
            // Example: {$and: [{a: 1, b: 1}, {$or: [...]}]}
            singleCompoundPredicate:
                singleKeyObjArb(fc.constantFrom(...compoundOps),
                                fc.array(tie('predicate'), {minLength: 1, maxLength: 3})),
            // Example: {a: {$eq: 5}}
            singleSimplePredicate: singleKeyObjArb(fieldArb, tie('condition')),
            // A single predicate is a simple predicate, or a compound predicate.
            singlePredicate: fc.oneof({withCrossShrink: true, maxDepth},
                                      tie('singleSimplePredicate'),
                                      tie('singleCompoundPredicate')),
            // For a full predicate model, we merge up to three single predicates.
            // Example: {a: {$eq: 1}, b: {$or: [...]}}
            predicate: fc.array(tie('singlePredicate'), {minLength: 1, maxLength: 3}).map(preds => {
                return Object.assign({}, ...preds);
            })
        };
    });
}

/*
 * Arbitrary $match expression that may contain simple comparisons, nested comparisons, $elemMatch,
 * and $not.
 * $or, $nor, $in and $nin are only allowed if `allowOrTypes` is true.
 */
export function getMatchArb(allowOrTypes = true) {
    const predicateArb = getMatchPredicateSpec({
                             leafArb: leafParameterArb,
                             allowOrs: allowOrTypes,
                             allowNors: allowOrTypes,
                             allowIn: allowOrTypes,
                             allowNin: allowOrTypes
                         }).predicate;
    return fc.record({$match: predicateArb});
}
