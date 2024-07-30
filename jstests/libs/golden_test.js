export function tojsonOnelineSortKeys(x) {
    let indent = " ";
    let nolint = true;
    let depth = undefined;
    let sortKeys = true;
    return tojson(x, indent, nolint, depth, sortKeys);
}

// Takes an array of documents.
// - Discards the field ordering, by recursively sorting the fields of each object.
// - Discards the result-set ordering by sorting the array of normalized documents.
// Returns a string.
export function normalize(result) {
    return result.map(d => tojsonOnelineSortKeys(d)).sort().join('\n') + '\n';
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

    print(normalize(cursorOrArray));
}
