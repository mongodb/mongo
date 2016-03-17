// SERVER-21488 Test that multi inserts into capped collections don't cause corruption.
// Note: this file must have a name that starts with "bulk" so it gets run by bulk_gle_passthrough.
(function() {
    "use strict";
    var t = db.capped_multi_insert;
    t.drop();

    db.createCollection(t.getName(), {capped: true, size: 16 * 1024, max: 1});

    t.insert([{_id: 1}, {_id: 2}]);
    assert.gleSuccess(db);

    // Ensure the collection is valid.
    var res = t.validate(true);
    assert(res.valid, tojson(res));

    // Ensure that various ways of iterating the collection only return one document.
    assert.eq(t.find().itcount(), 1);                             // Table scan.
    assert.eq(t.find({}, {_id: 1}).hint({_id: 1}).itcount(), 1);  // Index only (covered).
    assert.eq(t.find().hint({_id: 1}).itcount(), 1);              // Index scan with fetch.

    // Ensure that the second document is the one that is kept.
    assert.eq(t.findOne(), {_id: 2});
}());
