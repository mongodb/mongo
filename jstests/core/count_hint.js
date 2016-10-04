/**
 * Tests passing hint to the count command:
 *   - A hint should be respected even if it results in an incorrect count.
 *   - A bad argument to the hint() method should raise an error.
 *   - The hint() method should support both the name of the index, and the object spec of the
 *     index.
 */

(function() {
    "use strict";
    var coll = db.jstests_count_hint;
    coll.drop();

    assert.writeOK(coll.insert({i: 1}));
    assert.writeOK(coll.insert({i: 2}));

    assert.eq(2, coll.find().count());

    assert.commandWorked(coll.ensureIndex({i: 1}));

    assert.eq(2, coll.find().hint("i_1").count());
    assert.eq(2, coll.find().hint({i: 1}).count());

    assert.eq(1, coll.find({i: 1}).hint("_id_").count());
    assert.eq(1, coll.find({i: 1}).hint({_id: 1}).count());

    assert.eq(2, coll.find().hint("_id_").count());
    assert.eq(2, coll.find().hint({_id: 1}).count());

    // Create a sparse index which should have no entries.
    assert.commandWorked(coll.ensureIndex({x: 1}, {sparse: true}));

    // A hint should be respected, even if it results in the wrong answer.
    assert.eq(0, coll.find().hint("x_1").count());
    assert.eq(0, coll.find().hint({x: 1}).count());

    assert.eq(0, coll.find({i: 1}).hint("x_1").count());
    assert.eq(0, coll.find({i: 1}).hint({x: 1}).count());

    // SERVER-14792: bad hints should cause the count to fail, even if there is no query predicate.
    assert.throws(function() {
        coll.find().hint({bad: 1, hint: 1}).count();
    });
    assert.throws(function() {
        coll.find({i: 1}).hint({bad: 1, hint: 1}).count();
    });

    assert.throws(function() {
        coll.find().hint("BAD HINT").count();
    });
    assert.throws(function() {
        coll.find({i: 1}).hint("BAD HINT").count();
    });
})();
