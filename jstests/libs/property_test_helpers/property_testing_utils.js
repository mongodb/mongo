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
