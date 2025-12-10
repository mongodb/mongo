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
    leafParameterArb,
} from "jstests/libs/property_test_helpers/models/basic_models.js";
import {collationArb} from "jstests/libs/property_test_helpers/models/collation_models.js";
import {groupArb} from "jstests/libs/property_test_helpers/models/group_models.js";
import {getMatchArb} from "jstests/libs/property_test_helpers/models/match_models.js";
import {oneof} from "jstests/libs/property_test_helpers/models/model_utils.js";
import {fc} from "jstests/third_party/fast_check/fc-3.1.0.js";

// Inclusion/Exclusion projections. {$project: {_id: 1, a: 0}}
export function getSingleFieldProjectArb(isInclusion, {simpleFieldsOnly = false} = {}) {
    const projectedFieldArb = simpleFieldsOnly ? assignableFieldArb : fieldArb;
    return fc.record({field: projectedFieldArb, includeId: fc.boolean()}).map(function ({field, includeId}) {
        const includeIdVal = includeId ? 1 : 0;
        const includeFieldVal = isInclusion ? 1 : 0;
        return {$project: {_id: includeIdVal, [field]: includeFieldVal}};
    });
}
export const simpleProjectArb = oneof(
    getSingleFieldProjectArb(true /*isInclusion*/),
    getSingleFieldProjectArb(false /*isInclusion*/),
);

// Project from one field to another. {$project {a: '$b'}}
export const computedProjectArb = fc.tuple(fieldArb, dollarFieldArb).map(function ([destField, srcField]) {
    return {$project: {[destField]: srcField}};
});

// Add field with a constant argument. {$addFields: {a: 5}}
export const addFieldsConstArb = fc.tuple(fieldArb, leafParameterArb).map(function ([destField, leafParams]) {
    return {$addFields: {[destField]: leafParams}};
});
// Add field from source field. {$addFields: {a: '$b'}}
export const addFieldsVarArb = fc.tuple(fieldArb, dollarFieldArb).map(function ([destField, sourceField]) {
    return {$addFields: {[destField]: sourceField}};
});

/*
 * Generates a random $sort, with [1, maxNumSortComponents] sort components.
 *
 * `maxNumSortComponents` defaults to 1, because combining $sort on multiple fields with other
 * aggregation stages can lead to parallel key errors. For example
 *    [{$addFields: {a: '$array'}}, {$sort: {a: 1, array: 1}}]
 * attempts to sort on two array fields. This is not allowed in MQL.
 *
 * If the caller has guarantees about what stages will precede the $sort and can avoid parallel key
 * issues, they may set `maxNumSortComponents` to something greater than 1.
 */
export function getSortArb(maxNumSortComponents = 1) {
    const sortDirectionArb = fc.constantFrom(1, -1);
    const sortComponent = fc.record({field: fieldArb, dir: sortDirectionArb});
    return fc
        .uniqueArray(sortComponent, {
            minLength: 1,
            maxLength: maxNumSortComponents,
            selector: (fieldAndDir) => fieldAndDir.field,
        })
        .map((components) => {
            const sortSpec = {};
            for (const {field, dir} of components) {
                sortSpec[field] = dir;
            }
            return {$sort: sortSpec};
        });
}

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
function getAllowedStages(allowOrs, deterministicBag, isTS) {
    let allowedStages = [];
    const isTimeseriesCollection = TestData.isTimeseriesTestSuite || isTS;
    if (deterministicBag) {
        allowedStages = [
            simpleProjectArb,
            getMatchArb(allowOrs),
            addFieldsConstArb,
            computedProjectArb,
            addFieldsVarArb,
            getSortArb(),
        ];
    } else {
        // If we don't require a deterministic bag, we can allow $skip and $limit anywhere.
        allowedStages = [
            limitArb,
            skipArb,
            simpleProjectArb,
            getMatchArb(allowOrs),
            addFieldsConstArb,
            computedProjectArb,
            addFieldsVarArb,
            getSortArb(),
        ];
    }
    if (!isTimeseriesCollection) {
        allowedStages.push(groupArb);
    }
    return allowedStages;
}

/*
 * The pipeline arb generates a pipeline of stages.
 */
export function getAggPipelineArb({allowOrs = true, deterministicBag = true, allowedStages = [], isTS = false} = {}) {
    // TODO SERVER-83072 remove 'isTS' once $group timeseries array bug is fixed.
    const stages = allowedStages.length == 0 ? getAllowedStages(allowOrs, deterministicBag, isTS) : allowedStages;
    // Length 6 seems long enough to cover interactions between stages.
    return fc.array(oneof(...stages), {minLength: 1, maxLength: 6});
}

/*
 * Our full model for aggregation pipelines. The full model is an object that contains a pipeline
 * and options. See `getAllowedStages` for description of `allowOrs`
 * and `deterministicBag`. By default, ORs are allowed and the bag of results will be deterministic.
 * Allowed stages can be custom by the calling PBT if `allowedStages` is non-empty.
 */
export function getQueryAndOptionsModel({
    allowOrs = true,
    deterministicBag = true,
    allowCollation = false,
    allowedStages = [],
    isTS = false,
} = {}) {
    const noCollation = fc.constant({});
    return fc.record({
        "pipeline": getAggPipelineArb({allowOrs, deterministicBag, allowedStages, isTS}),
        "options": allowCollation ? oneof(noCollation, fc.record({"collation": collationArb})) : noCollation,
    });
}
