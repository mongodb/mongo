/**
 * Tests the dropIndexes command.
 */

import {IndexUtils} from "jstests/libs/index_utils.js";

const t = db.drop_indexes;
t.drop();

/**
 * Extracts index names from listIndexes result.
 */
function getIndexNames(cmdRes) {
    return t.getIndexes().map((spec) => spec.name);
}

/**
 * Checks that collection contains the given list of non-id indexes and nothing else.
 */
function assertIndexes(expectedIndexNames, msg) {
    const actualIndexNames = getIndexNames();
    const testMsgSuffix = () =>
        msg + ": expected " + tojson(expectedIndexNames) + " but got " + tojson(actualIndexNames) + " instead.";
    assert.eq(
        expectedIndexNames.length + 1,
        actualIndexNames.length,
        "unexpected number of indexes after " + testMsgSuffix(),
    );
    assert(actualIndexNames.includes("_id_"), "_id index missing after " + msg + ": " + tojson(actualIndexNames));
    for (let expectedIndexName of expectedIndexNames) {
        assert(
            actualIndexNames.includes(expectedIndexName),
            expectedIndexName + " index missing after " + testMsgSuffix(),
        );
    }
}

assert.commandWorked(t.insert({_id: 1, a: 2, b: 3, c: 1, d: 1, e: 1}));
IndexUtils.assertIndexes(t, [{_id: 1}], "inserting test document");

assert.commandWorked(t.createIndex({a: 1}));
assert.commandWorked(t.createIndex({b: 1}));
assert.commandWorked(t.createIndex({c: 1}));
assert.commandWorked(t.createIndex({d: 1}));
assert.commandWorked(t.createIndex({e: 1}));
IndexUtils.assertIndexes(t, [{_id: 1}, {a: 1}, {b: 1}, {c: 1}, {d: 1}, {e: 1}], "creating indexes");

// Drop multiple indexes.
assert.commandWorked(t.dropIndexes(["c_1", "d_1"]));
IndexUtils.assertIndexes(t, [{_id: 1}, {a: 1}, {b: 1}, {e: 1}], "dropping {c: 1} and {d: 1}");

// Must drop all the indexes provided or none at all - for example, if one of the index names
// provided is invalid.
let ex = assert.throws(() => {
    t.dropIndexes(["a_1", "_id_"]);
});
assert.commandFailedWithCode(ex, ErrorCodes.InvalidOptions);
IndexUtils.assertIndexes(t, [{_id: 1}, {a: 1}, {b: 1}, {e: 1}], "failed dropIndexes command with _id index");

// List of index names must contain only strings.
ex = assert.throws(() => {
    t.dropIndexes(["a_1", 123]);
});
assert.commandFailedWithCode(ex, ErrorCodes.TypeMismatch);
IndexUtils.assertIndexes(
    t,
    [{_id: 1}, {a: 1}, {b: 1}, {e: 1}],
    "failed dropIndexes command with non-string index name",
);
