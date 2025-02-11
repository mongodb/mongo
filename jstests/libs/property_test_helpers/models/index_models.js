/*
 * Fast-check models for indexes.
 */
import {fc} from "jstests/third_party/fast_check/fc-3.1.0.js";

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
const simpleIndexDefAndOptionsArb =
    fc.record({def: simpleIndexDefArb, options: simpleIndexOptionsArb});

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
const hashedIndexDefAndOptionsArb = fc.record({def: hashedIndexDefArb, options: fc.constant({})});

// Wildcard indexes. TODO SERVER-91164 expand coverage.
const wildcardIndexDefAndOptionsArb =
    fc.record({def: fc.constant({"$**": 1}), options: fc.constant({})});

// Map to an object with the definition and options, so it's more clear what each object is.
export const indexModel = fc.oneof(
    simpleIndexDefAndOptionsArb, wildcardIndexDefAndOptionsArb, hashedIndexDefAndOptionsArb);

function isMultikey(indexDef) {
    for (const field of Object.keys(indexDef)) {
        if (field === 'array') {
            return true;
        }
    }
    return false;
}
// Wildcard, hashed, sparse, and multikey indexes are not compatible with time-series collections.
export const timeseriesIndexModel = simpleIndexDefAndOptionsArb.filter(({def, options}) => {
    // Filter out any indexes that won't work for time-series.
    if (options.sparse || isMultikey(def)) {
        return false;
    }
    return true;
});
