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
    dollarFieldArb,
    fieldArb,
    leafParameterArb
} from "jstests/libs/property_test_helpers/models/basic_models.js";
import {groupArb} from "jstests/libs/property_test_helpers/models/group_models.js";
import {getMatchArb} from "jstests/libs/property_test_helpers/models/match_models.js";
import {oneof} from "jstests/libs/property_test_helpers/models/model_utils.js";
import {fc} from "jstests/third_party/fast_check/fc-3.1.0.js";

// Inclusion/Exclusion projections. {$project: {_id: 1, a: 0}}
export function getSingleFieldProjectArb(isInclusion, {simpleFieldsOnly = false} = {}) {
    const projectedFieldArb = simpleFieldsOnly ? assignableFieldArb : fieldArb;
    return fc.record({field: projectedFieldArb, includeId: fc.boolean()})
        .map(function({field, includeId}) {
            const includeIdVal = includeId ? 1 : 0;
            const includeFieldVal = isInclusion ? 1 : 0;
            return {$project: {_id: includeIdVal, [field]: includeFieldVal}};
        });
}
const projectArb = oneof(getSingleFieldProjectArb(true /*isInclusion*/),
                         getSingleFieldProjectArb(false /*isInclusion*/));

// Project from one field to another. {$project {a: '$b'}}
const computedProjectArb = fc.tuple(fieldArb, dollarFieldArb).map(function([destField, srcField]) {
    return {$project: {[destField]: srcField}};
});

// Add field with a constant argument. {$addFields: {a: 5}}
export const addFieldsConstArb =
    fc.tuple(fieldArb, leafParameterArb).map(function([destField, leafParams]) {
        return {$addFields: {[destField]: leafParams}};
    });
// Add field from source field. {$addFields: {a: '$b'}}
const addFieldsVarArb = fc.tuple(fieldArb, dollarFieldArb).map(function([destField, sourceField]) {
    return {$addFields: {[destField]: sourceField}};
});

export const sortArb = fc.tuple(fieldArb, fc.constantFrom(1, -1)).map(function([field, sortOrder]) {
    // If we sort on two or more fields, we run into the problem of sorting keys that
    // are parallel arrays. This is not allowed in MQL.
    return {$sort: {[field]: sortOrder}};
});

export const limitArb = fc.record({$limit: fc.integer({min: 1, max: 5})});
export const skipArb = fc.record({$skip: fc.integer({min: 1, max: 5})});

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
    const aggStageArb = oneof(...getAllowedStages(allowOrs, deterministicBag));
    // Length 6 seems long enough to cover interactions between stages.
    return fc.array(aggStageArb, {minLength: 0, maxLength: 6});
}
