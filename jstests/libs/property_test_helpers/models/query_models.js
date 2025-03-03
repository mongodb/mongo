/*
 * Fast-check models for aggregation pipelines for our core property tests.
 *
 * For our agg model, we generate query shapes with a list of concrete values the parameters could
 * take on at the leaves. We call this a "query family". This way, our properties have access to
 * many varying query shapes, but also variations of the same query shape.
 *
 * See property_test_helpers/README.md for more detail on the design.
 */
import {
    assignableFieldArb,
    fieldArb,
    scalarArb
} from "jstests/libs/property_test_helpers/models/basic_models.js";
import {fc} from "jstests/third_party/fast_check/fc-3.1.0.js";

export const leafParametersPerFamily = 10;
export class LeafParameter {
    constructor(concreteValues) {
        this.concreteValues = concreteValues;
    }
}

const leafParameterArb =
    fc.array(scalarArb, {minLength: 1, maxLength: leafParametersPerFamily}).map((constants) => {
        // In the leaves of the query family, we generate an object with a list of constants to
        // place.
        return new LeafParameter(constants);
    });

const dollarFieldArb = fieldArb.map(f => "$" + f);
const comparisonArb = fc.constantFrom('$eq', '$lt', '$lte', '$gt', '$gte');
const accumulatorArb =
    fc.constantFrom(undefined, '$count', '$min', '$max', '$minN', '$maxN', '$sum');

// Inclusion/Exclusion projections. {$project: {_id: 1, a: 0}}
const projectArb = fc.tuple(fieldArb, fc.boolean()).map(function([field, includeField]) {
    return {$project: {_id: 1, [field]: includeField}};
});
// Project from one field to another. {$project {a: '$b'}}
const computedProjectArb = fc.tuple(fieldArb, dollarFieldArb).map(function([destField, srcField]) {
    return {$project: {[destField]: srcField}};
});

// Add field with a constant argument. {$addFields: {a: 5}}
const addFieldsConstArb =
    fc.tuple(fieldArb, leafParameterArb).map(function([destField, leafParams]) {
        return {$addFields: {[destField]: leafParams}};
    });
// Add field from source field. {$addFields: {a: '$b'}}
const addFieldsVarArb = fc.tuple(fieldArb, dollarFieldArb).map(function([destField, sourceField]) {
    return {$addFields: {[destField]: sourceField}};
});

// Single leaf predicate of a $match. {a: {$eq: 5}}
const simpleMatchLeafPredicate =
    fc.tuple(fieldArb, comparisonArb, leafParameterArb).map(function([field, cmp, cmpValue]) {
        return {[field]: {[cmp]: cmpValue}};
    });
// {a: {$in: [1,2,3]}}
const inMatchPredicate =
    fc.tuple(fieldArb, fc.array(leafParameterArb, {minLength: 0, maxLength: 5}))
        .map(function([field, inVals]) {
            return {[field]: {$in: inVals}};
        });

// Arbitrary $match expression that may contain nested logical operations, or just leaves.
// {$match: {a: {$eq: 5}}}, {$match: {$and: [{a: {$eq: 5}}, {b: {$eq: 6}}]}}
// $or, $nor and $in are only allowed if `allowOrs` is true.
function getMatchArb(allowOrs) {
    const logicalOpArb = allowOrs ? fc.constantFrom('$and', '$or', '$nor') : fc.constant('$and');

    // $in is a form of OR, and shouldn't be generated when `allowOrs`=false.
    const matchLeafPredicate =
        allowOrs ? fc.oneof(simpleMatchLeafPredicate, inMatchPredicate) : simpleMatchLeafPredicate;
    const predicateArb =
        fc.letrec(
              tie => ({
                  compoundPred: fc.tuple(logicalOpArb,
                                         fc.array(tie('predicate'), {minLength: 1, maxLength: 3}))
                                    .map(([logicalOp, children]) => {
                                        return {[logicalOp]: children};
                                    }),
                  predicate: fc.oneof({maxDepth: 5}, matchLeafPredicate, tie('compoundPred'))
              }))
            .predicate;
    return fc.array(predicateArb, {minLength: 1, maxLength: 5}).map((predicates) => {
        // Merge all the predicates into one object.
        const mergedPredicates = Object.assign({}, ...predicates);
        return {$match: mergedPredicates};
    });
}

const sortArb = fc.tuple(fieldArb, fc.constantFrom(1, -1)).map(function([field, sortOrder]) {
    // TODO SERVER-91164 sort on multiple fields
    return {$sort: {[field]: sortOrder}};
});

// TODO SERVER-91164 include $top/$bottom and other accumulators, allow null as the groupby argument
// {$group: {_id: '$a', b: {$min: '$c'}}}
const groupArb =
    fc.tuple(
          dollarFieldArb, assignableFieldArb, accumulatorArb, dollarFieldArb, fc.integer({min: 1}))
        .map(function([gbField, outputField, acc, dataField, minMaxNumResults]) {
            if (acc === undefined) {
                // Simple $group with no accumulator
                return {$group: {_id: gbField}};
            }

            let accSpec;
            if (acc === '$count') {
                accSpec = {[acc]: {}};
            } else if (acc === '$minN' || acc === '$maxN') {
                accSpec = {[acc]: {input: dataField, n: minMaxNumResults}};
            } else {
                accSpec = {[acc]: dataField};
            }
            return {$group: {_id: gbField, [outputField]: accSpec}};
        });

const limitArb = fc.record({$limit: fc.integer({min: 1, max: 5})});
const skipArb = fc.record({$skip: fc.integer({min: 1, max: 5})});

/*
 * Return the arbitraries for agg stages that are allowed given:
 *    - `allowOrs` for whether we allow $or in $match
 *    - `deterministicBag` for whether the query needs to return the same bag of results every time.
 *       $limit and $skip prevent the bag from being consistent for each run, so we exclude these
 *       when a deterministic bag is required.
 * The output is in order from simplest agg stages to most complex, for minimization.
 */
function getAllowedStages(allowOrs, deterministicBag) {
    if (deterministicBag) {
        return [
            projectArb,
            getMatchArb(allowOrs),
            addFieldsConstArb,
            computedProjectArb,
            addFieldsVarArb,
            sortArb,
            groupArb
        ];
    } else {
        // If we don't require a deterministic bag, we can allow $skip and $limit anywhere.
        return [
            limitArb,
            skipArb,
            projectArb,
            getMatchArb(allowOrs),
            addFieldsConstArb,
            computedProjectArb,
            addFieldsVarArb,
            sortArb,
            groupArb
        ];
    }
}

/*
 * Our full model for aggregation pipelines. See `getAllowedStages` for description of `allowOrs`
 * and `deterministicBag`. By default, ORs are allowed and the bag of results will be deterministic.
 */
export function getAggPipelineModel({allowOrs = true, deterministicBag = true} = {}) {
    const aggStageArb = fc.oneof(...getAllowedStages(allowOrs, deterministicBag));
    // Length 6 seems long enough to cover interactions between stages.
    return fc.array(aggStageArb, {minLength: 0, maxLength: 6});
}
