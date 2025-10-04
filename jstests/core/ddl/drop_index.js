// Cannot implicitly shard accessed collections because of extra shard key index in sharded
// collection.
// @tags: [assumes_no_implicit_index_creation]
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

const t = db.drop_index;
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
assertIndexes([], "inserting test document");

assert.commandWorked(t.createIndex({a: 1}));
assert.commandWorked(t.createIndex({b: 1}));
assert.commandWorked(t.createIndex({c: 1}));
assert.commandWorked(t.createIndex({d: 1}));
assert.commandWorked(t.createIndex({e: 1}));
assertIndexes(["a_1", "b_1", "c_1", "d_1", "e_1"], "creating indexes");

// Drop single index by name.
// Collection.dropIndex() throws if the dropIndexes command fails.
assert.commandWorked(t.dropIndex(t._genIndexName({a: 1})));
assertIndexes(["b_1", "c_1", "d_1", "e_1"], "dropping {a: 1} by name");

// Drop single index by key pattern.
assert.commandWorked(t.dropIndex({b: 1}));
assertIndexes(["c_1", "d_1", "e_1"], "dropping {b: 1} by key pattern");

const runningOnMongos = FixtureHelpers.isMongos(db);

// Not allowed to drop _id index.
for (const dropIndexArg of ["_id_", {_id: 1}]) {
    const dropIdIndexReply = t.dropIndex(dropIndexArg);
    jsTestLog(`Reply to dropIndexes with arg ${tojson(dropIndexArg)}: ${tojson(dropIdIndexReply)}`);
    assert.commandFailedWithCode(dropIdIndexReply, ErrorCodes.InvalidOptions);
    assert(dropIdIndexReply.hasOwnProperty("errmsg"));
    if (runningOnMongos) {
        assert(dropIdIndexReply.hasOwnProperty("raw"));
    }
}

// Ensure you can recreate indexes, even if you don't use dropIndex method.
// Prior to SERVER-7168, the shell used to cache names of indexes created using
// Collection.createIndex().
assert.commandWorked(t.createIndex({a: 1}));
assertIndexes(["a_1", "c_1", "d_1", "e_1"], "recreating {a: 1}");

// Drop single index with dropIndexes().
assert.commandWorked(t.dropIndexes(["c_1"]));
assertIndexes(["a_1", "d_1", "e_1"], "dropping {c: 1}");
