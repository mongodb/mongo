/**
 * Tests that replacing a document with an equivalent document with different types for the fields
 * will update the index entries associated with that document.
 */

load("jstests/libs/analyze_plan.js");  // For 'isIndexOnly'.

(function() {
    "use strict";

    var coll = db.index_type_change;
    coll.drop();
    assert.commandWorked(coll.ensureIndex({a: 1}));

    assert.writeOK(coll.insert({a: 2}));
    assert.eq(1, coll.find({a: {$type: "double"}}).itcount());

    var newVal = new NumberLong(2);
    var res = coll.update({}, {a: newVal});  // Replacement update.
    assert.writeOK(res);
    assert.eq(res.nMatched, 1);
    if (coll.getMongo().writeMode() == "commands")
        assert.eq(res.nModified, 1);

    // Make sure it actually changed the type.
    assert.eq(1, coll.find({a: {$type: "long"}}).itcount());

    // Now use a covered query to ensure the index entry has been updated.

    // First make sure it's actually using a covered index scan.
    var explain = coll.explain().find({a: 2}, {_id: 0, a: 1});
    assert(isIndexOnly(explain));

    var updated = coll.findOne({a: 2}, {_id: 0, a: 1});

    assert(updated.a instanceof NumberLong, "Index entry did not change type");
})();
