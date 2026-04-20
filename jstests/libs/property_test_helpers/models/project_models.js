/*
 * Fast-check models for $project.
 */
import {assignableFieldArb, dollarFieldArb, fieldArb} from "jstests/libs/property_test_helpers/models/basic_models.js";
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

export function getMultipleFieldProjectArb(isInclusion = true, {simpleFieldsOnly = false} = {}) {
    // Choosing only from assignable fields to avoid projecting both m and m.1.
    const fieldVal = isInclusion ? 1 : 0;
    const projectedFieldArb = simpleFieldsOnly ? assignableFieldArb : fieldArb;
    // We cannot have both a field and its subfield in the same $project.
    function hasPathCollision(fields) {
        return fields.some((f) => fields.some((g) => g !== f && g.startsWith(f + ".")));
    }
    const arbArray = fc
        .uniqueArray(projectedFieldArb, {minLength: 2, maxLength: simpleFieldsOnly ? 4 : 6})
        .filter((fields) => !hasPathCollision(fields));
    return fc.record({fields: arbArray, includeId: fc.boolean()}).map(function ({fields, includeId}) {
        const projectSpec = {_id: includeId ? 1 : 0};
        for (const field of fields) {
            projectSpec[field] = fieldVal;
        }
        return {$project: projectSpec};
    });
}

export const multipleFieldProjectArb = oneof(
    getMultipleFieldProjectArb(true /*isInclusion*/),
    getMultipleFieldProjectArb(false /*isInclusion*/),
);

// Project from one field to another, parameterized on the dest and src field arbs.
// {$project: {[dest]: '$src'}}.
export function getComputedProjectArb(destFieldArb, srcFieldArb) {
    return fc.tuple(destFieldArb, srcFieldArb).map(function ([destField, srcField]) {
        return {$project: {[destField]: srcField}};
    });
}
export const computedProjectArb = getComputedProjectArb(fieldArb, dollarFieldArb);
