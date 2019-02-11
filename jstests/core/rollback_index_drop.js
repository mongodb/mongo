// Test that when an index drop rolls back, the index remains valid, and the server continues to
// correctly maintain the index's set of keys.
//
// This test was designed to reproduce SERVER-38372.
//
// @tags: [does_not_support_stepdowns, assumes_unsharded_collection]
(function() {
    "use strict";

    const coll = db.rollback_index_drop;
    coll.drop();

    assert.commandWorked(coll.insert([{a: 1}, {a: 2}, {a: 3}]));
    assert.commandWorked(coll.createIndex({a: 1}));

    // Verify that the index has the expected set of keys.
    assert.eq([{a: 1}, {a: 2}, {a: 3}],
              coll.find().hint({a: 1}).sort({a: 1}).returnKey().toArray());

    // Run a dropIndexes command that attempts to drop both {a: 1} and an invalid index. This should
    // cause the drop of {a: 1} to rollback, since the set of index drops happen atomically.
    assert.commandFailedWithCode(
        db.runCommand({dropIndexes: coll.getName(), index: ["a_1", "unknown"]}),
        ErrorCodes.IndexNotFound);

    // Verify that the {a: 1} index is still present in listIndexes output.
    const indexList = coll.getIndexes();
    assert.neq(undefined, indexList.find((idx) => idx.name === "a_1"), indexList);

    // Write to the collection and ensure that the resulting set of index keys is correct.
    assert.commandWorked(coll.update({a: 3}, {$inc: {a: 1}}));
    assert.commandWorked(coll.insert({a: 5}));
    assert.eq([{a: 1}, {a: 2}, {a: 4}, {a: 5}],
              coll.find().hint({a: 1}).sort({a: 1}).returnKey().toArray());
}());
