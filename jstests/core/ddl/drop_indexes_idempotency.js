/**
 * Test that dropIndexes successfully completes when an index doesn't exist.
 *
 * @tags: [
 * requires_fcv_83,
 * # TODO SERVER-107101: Remove when dropIndexes supports balancer
 * assumes_balancer_off,
 * ]
 */

function assertIndexesExist(coll, indexes) {
    const indexList = coll.getIndexes();
    indexes.forEach((index) => {
        assert.neq(
            undefined,
            indexList.find((idx) => idx.name === index),
            `Index ${index} was expected to exist but it doesn't`,
        );
    });
}

function assertIndexesDontExist(coll, indexes) {
    const indexList = coll.getIndexes();
    indexes.forEach((index) => {
        assert.eq(
            undefined,
            indexList.find((idx) => idx.name === index),
            `Index ${index} was expected to be absent, but it exists.`,
        );
    });
}

const coll = db.partial_index_drop;
coll.drop();

assert.commandWorked(coll.insert([{a: 1}, {a: 2}, {a: 3}]));
assert.commandWorked(coll.createIndex({a: 1}));
assert.commandWorked(coll.createIndex({b: 1}));

// Verify that both indexes exist
let indexList = coll.getIndexes();
assertIndexesExist(coll, ["a_1", "b_1"]);

// Verify that the {a: 1} index has the expected set of keys.
assert.eq([{a: 1}, {a: 2}, {a: 3}], coll.find().hint({a: 1}).sort({a: 1}).returnKey().toArray());

// Run a dropIndexes command that attempts to drop both {a: 1} (exists) and "unknown" (doesn't
// exist). With the new behavior, this should succeed - dropping {a: 1} and ignoring "unknown".
assert.commandWorked(db.runCommand({dropIndexes: coll.getName(), index: ["a_1", "unknown"]}));

// Verify that the {a: 1} index has been dropped from listIndexes output.
indexList = coll.getIndexes();
assertIndexesDontExist(coll, ["a_1"]);

// Verify that the {b: 1} index is still present (wasn't requested to be dropped).
assertIndexesExist(coll, ["b_1"]);

// Test another scenario: drop multiple existing indexes
assert.commandWorked(coll.createIndex({c: 1}));
assert.commandWorked(coll.createIndex({d: 1}));

// Drop both existing indexes
assert.commandWorked(db.runCommand({dropIndexes: coll.getName(), index: ["b_1", "c_1", "d_1"]}));

// Verify all were dropped
indexList = coll.getIndexes();
assertIndexesDontExist(coll, ["b_1", "c_1", "d_1"]);

// Test edge case: try to drop only non-existent indexes
assert.commandWorked(db.runCommand({dropIndexes: coll.getName(), index: ["nonexistent1", "nonexistent2"]}));
