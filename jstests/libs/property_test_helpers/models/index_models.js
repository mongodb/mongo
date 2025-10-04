/*
 * Fast-check models for indexes.
 * See property_test_helpers/README.md for more detail on the design.
 */
import {fieldArb, getScalarArb} from "jstests/libs/property_test_helpers/models/basic_models.js";
import {getPartialFilterPredicateArb} from "jstests/libs/property_test_helpers/models/match_models.js";
import {oneof} from "jstests/libs/property_test_helpers/models/model_utils.js";
import {fc} from "jstests/third_party/fast_check/fc-3.1.0.js";

/*
 * Takes an object, a position in that object, and a new key/value. Returns a new object where the
 * key/val at the specified position is replaced with the new key/val. If no new key is provided,
 * the existing key is kept. If no new value is provided, the existing value is kept.
 *
 * Example: {a: 1, b: 1, c: 1}, ix=1, newKey="b2"
 * Output:  {a: 1, b2: 1, c: 1}
 *
 * Example: {a: 1, b: 1, c: 1}, ix=2, newValue=2
 * Output:  {a: 1, b: 1, c: 2}
 *
 * Example: {a: 1, b: 1, c: 1}, ix=0, newKey="a2", newValue=0
 * Output:  {a2: 0, b: 1, c: 1}
 */
function replaceKeyValAtPosition(obj, ix, {newKey, newVal}) {
    const keys = Object.keys(obj);
    const values = Object.values(obj);

    assert(0 <= ix && ix < keys.length);
    assert(newKey || newVal);
    if (!newKey) {
        newKey = keys[ix];
    }
    if (!newVal) {
        newVal = values[ix];
    }

    const newObj = {};
    for (let i = 0; i < keys.length; i++) {
        if (i === ix) {
            newObj[newKey] = newVal;
        } else {
            newObj[keys[i]] = values[i];
        }
    }
    return newObj;
}

// Regular indexes
// Tuple of indexed field, and it's sort direction.
const singleIndexDefArb = fc.record({field: fieldArb, dir: fc.constantFrom(1, -1)});
// Unique array of [[a, true], [b, false], ...] to be mapped to an index definition. Unique on the
// indexed field. Filter out any indexes that only use the _id field.
const arrayOfSingleIndexDefsArb = fc
    .uniqueArray(singleIndexDefArb, {
        minLength: 1,
        maxLength: 5,
        selector: (fieldAndSort) => fieldAndSort.field,
    })
    .filter((arrayOfIndexDefs) => {
        // We can run into errors if we try to make an {_id: -1} index.
        if (arrayOfIndexDefs.length === 1 && arrayOfIndexDefs[0].field === "_id") {
            return false;
        }
        return true;
    });
const simpleIndexDefArb = arrayOfSingleIndexDefsArb.map((arrayOfIndexDefs) => {
    // Convert to a valid index definition structure.
    const fullDef = {};
    for (const {field, dir} of arrayOfIndexDefs) {
        fullDef[field] = dir;
    }
    return fullDef;
});

const emptyOptionsArb = fc.constant({});

// Generates an index option for partial filters. Since predicate models by default have a list of
// parameters at their leaves, we specify we want a single scalar at the leaves.
const partialFilterOptionArb = getPartialFilterPredicateArb({leafArb: getScalarArb()}).map((pred) => {
    return {partialFilterExpression: pred};
});

/*
 * A b-tree index model.
 */
function getSimpleIndexModel({allowPartialIndexes, allowSparse}) {
    const options = [emptyOptionsArb];
    if (allowSparse) {
        options.push(fc.constant({sparse: true}));
    }
    if (allowPartialIndexes) {
        options.push(partialFilterOptionArb);
    }
    return fc.record({def: simpleIndexDefArb, options: oneof(...options)});
}

/*
 * Hashed indexes
 * Generate a simple index definition, an position into that definition, and replace the value at
 * that position with the value 'hashed'
 */
const hashedIndexDefArb = fc
    .record({indexDef: simpleIndexDefArb, hashedIx: fc.integer({min: 0, max: 4 /* Inclusive */})})
    .map(({indexDef, hashedIx}) => {
        hashedIx %= Object.keys(indexDef).length;
        return replaceKeyValAtPosition(indexDef, hashedIx, {newVal: "hashed"});
    })
    .filter((fullDef) => {
        // Can't create hashed index on array field.
        return !Object.keys(fullDef).includes("array");
    });

function getHashedIndexModel(allowPartialIndexes) {
    const optionsArb = allowPartialIndexes ? oneof(emptyOptionsArb, partialFilterOptionArb) : emptyOptionsArb;
    return fc.record({def: hashedIndexDefArb, options: optionsArb});
}

// This models wildcard indexes where the wildcard field is at the top-level, like "$**" rather than
// "a.$**". These definitions are allowed to specify a `wildcardProjection` in the index options.
const wildcardProjectionOptionsArb = fc.record({
    wildcardProjection: fc.uniqueArray(fieldArb, {minLength: 1, maxLength: 8}).map((fields) => {
        const options = {};
        for (const field of fields) {
            options[field] = 1;
        }
        return options;
    }),
});

/*
 * Generate a simple index definition, a position into that definition, and replace the key at the
 * position with '$**'.
 */
const fullWildcardDefArb = fc
    .record({
        indexDef: simpleIndexDefArb,
        wcIx: fc.integer({min: 0, max: 4}),
    })
    .map(({indexDef, wcIx}) => {
        wcIx %= Object.keys(indexDef).length;
        return replaceKeyValAtPosition(indexDef, wcIx, {newKey: "$**"});
    });

/*
 * Models a wildcard index where the wildcard field is not at the top-level. So for example "a.$**".
 * Generate a simple index definition, a position into that definition, a field, and replace the key
 * at the position with `field + '.$**'`.
 */
const dottedWildcardDefArb = fc
    .record({
        indexDef: simpleIndexDefArb,
        fieldPrefix: fieldArb,
        wcIx: fc.integer({min: 0, max: 4}),
    })
    .map(({indexDef, fieldPrefix, wcIx}) => {
        wcIx %= Object.keys(indexDef).length;
        const wcFieldName = fieldPrefix + ".$**";
        return replaceKeyValAtPosition(indexDef, wcIx, {newKey: wcFieldName});
    });

function isMultikey(indexDef) {
    for (const field of Object.keys(indexDef)) {
        if (field === "array") {
            return true;
        }
    }
    return false;
}

/*
 * A wildcard index can be at the top-level (fullWildcardDef) or on a field (dottedWildcardDef).
 */
function getWildCardIndexModel(allowPartialIndexes) {
    // Full wildcard options contain a wildcard projection, and possibly a partial filter if partial
    // indexes are allowed.
    let fullWcOptionsArb;
    if (allowPartialIndexes) {
        fullWcOptionsArb = fc
            .record({
                wcOptions: wildcardProjectionOptionsArb,
                partialFilterOptions: fc.option(partialFilterOptionArb, {nil: {}}),
            })
            .map(({wcOptions, partialFilterOptions}) => {
                return Object.assign({}, wcOptions, partialFilterOptions);
            });
    } else {
        fullWcOptionsArb = wildcardProjectionOptionsArb;
    }
    const fullWcModel = fc.record({def: fullWildcardDefArb, options: fullWcOptionsArb});

    // Dotted wildcard indexes don't allow wildcard projection options, but can be partial if that's
    // allowed.
    const dottedWcOptionsArb = allowPartialIndexes ? oneof(emptyOptionsArb, partialFilterOptionArb) : emptyOptionsArb;
    const dottedWcModel = fc.record({def: dottedWildcardDefArb, options: dottedWcOptionsArb});

    return oneof(fullWcModel, dottedWcModel).filter(({def, options}) => {
        // Wildcard indexes are not allowed to be multikey.
        return !isMultikey(def);
    });
}

/*
 * `getIndexModel` and `getTimeSeriesIndexModel` return index models for their respective collection
 * types. Takes arguments to allow or disallow partial indexes and sparse indexes.
 *
 * Partial indexes are disabled by default because they tend to lead to indexes not being used. It's
 * unlikely for a query to begin with a $match that is a subset of a partial index filter just by
 * coincidence. With partial indexes being generated, we're more likely to not find an index to use
 * and fall back to a collscan which is a less interesting case to test.
 *
 * Wildcard, hashed, sparse, and multikey indexes are not compatible with time-series collections.
 */
export function getIndexModel({allowPartialIndexes = false, allowSparse = true} = {}) {
    return oneof(
        getSimpleIndexModel({allowPartialIndexes, allowSparse}),
        getWildCardIndexModel(allowPartialIndexes),
        getHashedIndexModel(allowPartialIndexes),
    );
}
export function getTimeSeriesIndexModel({allowPartialIndexes = false} = {}) {
    // TODO SERVER-102738 support more time-series index types.
    const simpleIndexModel = getSimpleIndexModel({allowPartialIndexes, allowSparse: false});
    return simpleIndexModel.filter(({def, options}) => {
        // Filter out multikey indexes.
        return !isMultikey(def);
    });
}
