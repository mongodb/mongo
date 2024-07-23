export function tojsonOnelineSortKeys(x) {
    return tojson(x, " " /*indent*/, true /*nolint*/, undefined /*depth*/, true /*sortKeys*/);
}

export function tojsonMultiLineSortKeys(x) {
    return tojson(
        x, undefined /*indent*/, false /*nolint*/, undefined /*depth*/, true /*sortKeys*/);
}

// Takes an array of documents ('result').
// If `shouldSort` is true:
//    - Discards the field ordering, by recursively sorting the fields of each object.
//    - Discards the result-set ordering by sorting the array of normalized documents.
// Returns a string.
export function normalizeArray(result, shouldSort = true) {
    if (!Array.isArray(result)) {
        throw Error("The result is not an array: " + tojson(result));
    }

    const normalizedResults = shouldSort ? result.map(d => tojsonOnelineSortKeys(d)).sort()
                                         : result.map(d => tojsononeline(d));
    return normalizedResults.join('\n') + '\n';
}

// Takes an array or cursor, and prints a normalized version of it.
//
// Normalizing means ignoring:
// - order of fields in a document
// - order of documents in the array/cursor.
//
// If running the query fails, this catches and prints the exception.
export function show(cursorOrArray) {
    if (!Array.isArray(cursorOrArray)) {
        try {
            cursorOrArray = cursorOrArray.toArray();
        } catch (e) {
            print(tojson(e));
            return;
        }
    }

    print(normalizeArray(cursorOrArray));
}
