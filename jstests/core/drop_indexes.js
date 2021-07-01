// Cannot implicitly shard accessed collections because of extra shard key index in sharded
// collection. Cannot be handled correctly in a stepdown suite since dropIndexes() with multiple
// names cannot be retried properly.
// @tags: [assumes_no_implicit_index_creation, does_not_support_stepdowns]
(function() {
'use strict';

const t = db.drop_indexes;
t.drop();

/**
 * Extracts index names from listIndexes result.
 */
function getIndexNames(cmdRes) {
    return t.getIndexes().map(spec => spec.name);
}

/**
 * Checks that collection contains the given list of non-id indexes and nothing else.
 */
function assertIndexes(expectedIndexNames, msg) {
    const actualIndexNames = getIndexNames();
    const testMsgSuffix = () => msg + ': expected ' + tojson(expectedIndexNames) + ' but got ' +
        tojson(actualIndexNames) + ' instead.';
    assert.eq(expectedIndexNames.length + 1,
              actualIndexNames.length,
              'unexpected number of indexes after ' + testMsgSuffix());
    assert(actualIndexNames.includes('_id_'),
           '_id index missing after ' + msg + ': ' + tojson(actualIndexNames));
    for (let expectedIndexName of expectedIndexNames) {
        assert(actualIndexNames.includes(expectedIndexName),
               expectedIndexName + ' index missing after ' + testMsgSuffix());
    }
}

assert.commandWorked(t.insert({_id: 1, a: 2, b: 3, c: 1, d: 1, e: 1}));
assertIndexes([], 'inserting test document');

assert.commandWorked(t.createIndex({a: 1}));
assert.commandWorked(t.createIndex({b: 1}));
assert.commandWorked(t.createIndex({c: 1}));
assert.commandWorked(t.createIndex({d: 1}));
assert.commandWorked(t.createIndex({e: 1}));
assertIndexes(['a_1', 'b_1', 'c_1', 'd_1', 'e_1'], 'creating indexes');

// Drop multiple indexes.
assert.commandWorked(t.dropIndexes(['c_1', 'd_1']));
assertIndexes(['a_1', 'b_1', 'e_1'], 'dropping {c: 1} and {d: 1}');

// Must drop all the indexes provided or none at all - for example, if one of the index names
// provided is invalid.
let ex = assert.throws(() => {
    t.dropIndexes(['a_1', '_id_']);
});
assert.commandFailedWithCode(ex, ErrorCodes.InvalidOptions);
assertIndexes(['a_1', 'b_1', 'e_1'], 'failed dropIndexes command with _id index');

// List of index names must contain only strings.
ex = assert.throws(() => {
    t.dropIndexes(['a_1', 123]);
});
assert.commandFailedWithCode(ex, ErrorCodes.TypeMismatch);
assertIndexes(['a_1', 'b_1', 'e_1'], 'failed dropIndexes command with non-string index name');
}());
