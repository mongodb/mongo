/*
 * Models for object $elemMatch traversal, shared by the elemmatch_object_*_pbt.js tests.
 */
import {
    getScalarArb,
    leafParameterArb,
} from "jstests/libs/property_test_helpers/models/basic_models.js";
import {getDatasetModel} from "jstests/libs/property_test_helpers/models/document_models.js";
import {oneof} from "jstests/libs/property_test_helpers/models/model_utils.js";
import {fc} from "jstests/third_party/fast_check/fc-3.1.0.js";

const scalarArb = getScalarArb({allowUnicode: false, allowNullBytes: false});

const elemObjArb = fc.record(
    {
        // A scalar, a sub-object, an array of scalars and an array of objects.
        // Together these give the paths "arr.b", "arr.c.d", "arr.e" and "arr.f.g".
        b: scalarArb,
        c: fc.record({d: scalarArb}, {requiredKeys: []}),
        e: fc.array(scalarArb, {maxLength: 2}),
        f: fc.array(fc.record({g: scalarArb}, {requiredKeys: []}), {maxLength: 2}),
    },
    {requiredKeys: []},
);

const elemArb = oneof(
    {arbitrary: elemObjArb, weight: 4},
    {arbitrary: scalarArb, weight: 1},
    {arbitrary: fc.array(scalarArb, {maxLength: 2}), weight: 1},
);

/*
 * Documents with one array-of-objects field "arr", plus the scalar fields "a" and "w" which can be
 * used as the leading keys of a compound index.
 */
export function getElemMatchDocsModel({maxNumDocs = 100} = {}) {
    const docModel = fc.record({
        a: scalarArb,
        w: fc.boolean(),
        arr: fc.array(elemArb, {maxLength: 4}),
    });
    return getDatasetModel({docModel, maxNumDocs});
}

const leafConditionArb = oneof(
    fc
        .tuple(fc.constantFrom("$eq", "$ne", "$lt", "$lte", "$gt", "$gte"), leafParameterArb)
        .map(([comparator, leaf]) => ({[comparator]: leaf})),
    fc.record({$in: fc.array(leafParameterArb, {maxLength: 3})}),
    fc.record({$nin: fc.array(leafParameterArb, {maxLength: 3})}),
    fc.record({$exists: fc.boolean()}),
);

// Generate $not more often, because this is a risky area
const conditionArb = oneof(
    {arbitrary: leafConditionArb, weight: 3},
    {arbitrary: fc.record({$not: leafConditionArb}), weight: 2},
);

// Drop duplicate sub-fields.
function elemMatchBodyArb(subFields) {
    return fc
        .array(
            fc
                .tuple(fc.constantFrom(...subFields), conditionArb)
                .map(([field, condition]) => ({[field]: condition})),
            {minLength: 1, maxLength: 3},
        )
        .map((pairs) => Object.assign({}, ...pairs));
}

/*
 * A single $elemMatch predicate. Every sub-field named here has a matching index key in
 * getElemMatchIndexesModel(), so each generated predicate has some index that could serve it.
 */
export const elemMatchPredicateArb = oneof(
    {
        arbitrary: elemMatchBodyArb(["b", "c.d", "e", "f.g"]).map((body) => ({
            arr: {$elemMatch: body},
        })),
        weight: 3,
    },
    // Compare on arr.f directly
    {
        arbitrary: elemMatchBodyArb(["g"]).map((body) => ({"arr.f": {$elemMatch: body}})),
        weight: 1,
    },
    // Compare on arr.e directly
    {arbitrary: conditionArb.map((condition) => ({"arr.e": {$elemMatch: condition}})), weight: 1},
);

// A predicate on a field outside the array. Used as the leading key of a compound index, or as the
// other branch of an $or.
export const nonArrayFieldPredicateArb = fc
    .tuple(fc.constantFrom("a", "w", "_id"), conditionArb)
    .map(([field, condition]) => ({[field]: condition}));

const arrayIndexPaths = ["arr.b", "arr.c.d", "arr.e", "arr.f.g"];
// "e" and "f" are two arrays in the same element, so one index cannot contain a key from both.
const parallelArrayPaths = ["arr.e", "arr.f.g"];
const indexFieldArb = fc.constantFrom("a", "w", "_id", ...arrayIndexPaths);

const randomIndexArb = fc.record({
    def: fc
        .uniqueArray(fc.record({field: indexFieldArb, dir: fc.constantFrom(1, -1)}), {
            minLength: 1,
            maxLength: 4,
            selector: (fieldAndDir) => fieldAndDir.field,
        })
        .filter((defs) => defs.some((d) => arrayIndexPaths.includes(d.field)))
        .filter((defs) => defs.filter((d) => parallelArrayPaths.includes(d.field)).length <= 1)
        .map((defs) => Object.fromEntries(defs.map(({field, dir}) => [field, dir]))),
    options: fc.record({sparse: fc.boolean()}, {requiredKeys: []}),
});

/*
 * Indexes for the generated documents. The first four are always present so that every predicate
 * from elemMatchPredicateArb has an index available.
 */
export function getElemMatchIndexesModel() {
    const fixedIndexes = [
        {def: {"arr.b": 1}, options: {}},
        {def: {"arr.c.d": 1}, options: {}},
        {def: {"arr.e": 1}, options: {}},
        {def: {"arr.f.g": 1}, options: {}},
        {def: {a: 1, w: 1, "arr.c.d": 1, "arr.b": 1}, options: {}},
    ];
    return fc
        .array(randomIndexArb, {minLength: 1, maxLength: 5, size: "+2"})
        .map((randomIndexes) => [...fixedIndexes, ...randomIndexes]);
}

/*
 * Check if any part of `node` negates: an explicit $not, a $ne or $nin, or {$exists: false}.
 */
export function hasNegation(node) {
    if (Array.isArray(node)) {
        return node.some(hasNegation);
    }
    if (node === null || typeof node !== "object") {
        return false;
    }
    for (const [key, val] of Object.entries(node)) {
        if (key === "$not" || key === "$ne" || key === "$nin") {
            return true;
        }
        if (key === "$exists" && val === false) {
            return true;
        }
        if (hasNegation(val)) {
            return true;
        }
    }
    return false;
}
