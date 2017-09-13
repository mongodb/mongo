// Tests for $natural sort and $natural hint.
(function() {
    'use strict';

    var results;

    var coll = db.jstests_natural;
    coll.drop();

    assert.commandWorked(coll.ensureIndex({a: 1}));
    assert.writeOK(coll.insert({_id: 1, a: 3}));
    assert.writeOK(coll.insert({_id: 2, a: 2}));
    assert.writeOK(coll.insert({_id: 3, a: 1}));

    // Regression test for SERVER-20660. Ensures that documents returned with $natural don't have
    // any extraneous fields.
    results = coll.find({a: 2}).sort({$natural: 1}).toArray();
    assert.eq(results.length, 1);
    assert.eq(results[0], {_id: 2, a: 2});

    // Regression test for SERVER-20660. Ensures that documents returned with $natural don't have
    // any extraneous fields.
    results = coll.find({a: 2}).hint({$natural: -1}).toArray();
    assert.eq(results.length, 1);
    assert.eq(results[0], {_id: 2, a: 2});
})();
