import {LeafParameter} from "jstests/libs/property_test_helpers/query_models.js";

/*
 * Given a query family and an index in [0-numLeafParameters), we replace the leaves of the query
 * with the corresponding constant at that index.
 */
export function concreteQueryFromFamily(queryShape, leafId) {
    if (queryShape instanceof LeafParameter) {
        // We found a leaf, and want to return a concrete constant instead.
        // The leaf node should have one key, and the value should be our constants.
        const vals = queryShape.concreteValues;
        return vals[leafId % vals.length];
    } else if (Array.isArray(queryShape)) {
        // Recurse through the array, replacing each leaf with a value.
        const result = [];
        for (const el of queryShape) {
            result.push(concreteQueryFromFamily(el, leafId));
        }
        return result;
    } else if (typeof queryShape === 'object' && queryShape !== null) {
        // Recurse through the object values and create a new object.
        const obj = {};
        const keys = Object.keys(queryShape);
        for (const key of keys) {
            obj[key] = concreteQueryFromFamily(queryShape[key], leafId);
        }
        return obj;
    }
    return queryShape;
}

/*
 * Default documents to use for the core PBT model schema.
 * TODO SERVER-93816 remove this function and model documents as an arbitrary so that documents can
 * be minimized.
 */
export function defaultPbtDocuments() {
    const datePrefix = 1680912440;
    const alphabet = 'abcdefghijklmnopqrstuvwxyz';
    const docs = [];
    let id = 0;
    for (let m = 0; m < 10; m++) {
        let currentDate = 0;
        for (let i = 0; i < 10; i++) {
            docs.push({
                _id: id,
                t: new Date(datePrefix + currentDate - 100),
                m: {m1: m, m2: 2 * m},
                array: [i, i + 1, 2 * i],
                a: NumberInt(10 - i),
                b: alphabet.charAt(i)
            });
            currentDate += 25;
            id += 1;
        }
    }
    return docs;
}
