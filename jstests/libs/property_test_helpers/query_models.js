/*
 * Fast-check models for aggregation pipelines and index definitions. Works for time-series
 * collections but also is general enough for regular collections.
 *
 * For our agg model, we generate query shapes with a list of concrete values the parameters could
 * take on at the leaves. We call this a "query family". This way, our properties have access to
 * many varying query shapes, but also variations of the same query shape.
 */

import {fc} from "jstests/third_party/fast_check/fc-3.1.0.js";

// ------------------------------------- Aggregation Arbitraries -----------------------------------
// .oneof() arguments are ordered from least complex to most, since fast-check uses this ordering to
// shrink.
const scalarArb = fc.oneof(fc.constant(null),
                           fc.boolean(),
                           fc.integer({min: -99, max: 99}),
                           // Strings starting with `$` can be confused with fields.
                           fc.string().filter(s => !s.startsWith('$')),
                           fc.date());
export const maxNumLeafParametersPerFamily = 10;
export class LeafParameter {
    constructor(concreteValues) {
        this.concreteValues = concreteValues;
    }
}

const leafParameterArb = fc.array(scalarArb, {
                               minLength: 1,
                               maxLength: maxNumLeafParametersPerFamily
                           }).map((constants) => {
    // In the leaves of the query family, we generate an object with a list of constants to place.
    return new LeafParameter(constants);
});

const fieldArb = fc.constantFrom('t', 'm', 'm.m1', 'm.m2', 'a', 'b', 'array');
const assignableFieldArb = fc.constantFrom('m', 't', 'a', 'b');
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
const matchLeafPredicate = fc.oneof(simpleMatchLeafPredicate, inMatchPredicate);

// Arbitrary $match expression that may contain nested logical operations, or just leaves.
// {$match: {a: {$eq: 5}}}, {$match: {$and: [{a: {$eq: 5}}, {b: {$eq: 6}}]}}
// $or and $nor are only allowed if `allowOrs` is true.
function getMatchArb(allowOrs) {
    const logicalOpArb = allowOrs ? fc.constantFrom('$and', '$or', '$nor') : fc.constant('$and');
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

// Arbitrary for single stage.
function getAggStageArb(allowOrs) {
    // TODO SERVER-91164 include $limit
    return fc.oneof(projectArb,
                    getMatchArb(allowOrs),
                    addFieldsConstArb,
                    computedProjectArb,
                    addFieldsVarArb,
                    sortArb,
                    groupArb);
}

// Our full model for aggregation pipelines. Length 6 seems long enough to cover interactions
// between stages.
export const aggPipelineModel =
    fc.array(getAggStageArb(true /* allowOrs */), {minLength: 0, maxLength: 6});
export const aggPipelineNoOrsModel =
    fc.array(getAggStageArb(false /* allowOrs */), {minLength: 0, maxLength: 6});

// ------------------------------------- Index Def Arbitraries -----------------------------------
const indexFieldArb = fc.constantFrom('_id', 't', 'm', 'm.m1', 'm.m2', 'a', 'b', 'array');

// Regular indexes
// Tuple of indexed field, and it's sort direction.
const singleIndexDefArb = fc.tuple(indexFieldArb, fc.constantFrom(1, -1));
// Unique array of [[a, true], [b, false], ...] to be mapped to an index definition. Unique on the
// indexed field. Filter out any indexes that only use the _id field.
const arrayOfSingleIndexDefsArb = fc.uniqueArray(singleIndexDefArb, {
                                        minLength: 1,
                                        maxLength: 5,
                                        selector: fieldAndSort => fieldAndSort[0],
                                    }).filter(arrayOfIndexDefs => {
    // We can run into errors if we try to make an {_id: -1} index.
    if (arrayOfIndexDefs.length === 1 && arrayOfIndexDefs[0][0] === '_id') {
        return false;
    }
    return true;
});
const simpleIndexDefArb = arrayOfSingleIndexDefsArb.map(arrayOfIndexDefs => {
    // Convert to a valid index definition structure.
    let fullDef = {};
    for (const [field, sortDirection] of arrayOfIndexDefs) {
        fullDef = Object.assign(fullDef, {[field]: sortDirection});
    }
    return fullDef;
});
const simpleIndexOptionsArb = fc.constantFrom({}, {sparse: true});
const simpleIndexDefAndOptionsArb = fc.tuple(simpleIndexDefArb, simpleIndexOptionsArb);

// Hashed indexes
const hashedIndexDefArb =
    fc.tuple(arrayOfSingleIndexDefsArb, fc.integer({min: 0, max: 4 /* Inclusive */}))
        .map(([arrayOfIndexDefs, positionOfHashed]) => {
            // Inputs are an index definition, and the position of the hashed field in the index
            // def.
            positionOfHashed %= arrayOfIndexDefs.length;
            let fullDef = {};
            let i = 0;
            for (const [field, sortDir] of arrayOfIndexDefs) {
                const sortDirOrHashed = i === positionOfHashed ? 'hashed' : sortDir;
                fullDef = Object.assign(fullDef, {[field]: sortDirOrHashed});
                i++;
            }
            return fullDef;
        })
        .filter(fullDef => {
            // Can't create hashed index on array field.
            return !Object.keys(fullDef).includes('array');
        });
// No index options for hashed or wildcard indexes.
const hashedIndexDefAndOptionsArb = fc.tuple(hashedIndexDefArb, fc.constant({}));

// Wildcard indexes. TODO SERVER-91164 expand coverage.
const wildcardIndexDefAndOptionsArb = fc.tuple(fc.constant({"$**": 1}), fc.constant({}));

// Map to an object with the definition and options, so it's more clear what each object is.
const indexInfoArb =
    fc.oneof(
          simpleIndexDefAndOptionsArb, wildcardIndexDefAndOptionsArb, hashedIndexDefAndOptionsArb)
        .map(([def, options]) => {
            return {def, options};
        });
export const listOfIndexesModel = fc.array(indexInfoArb, {minLength: 0, maxLength: 7});
