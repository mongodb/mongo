// Tests for predicates which can use the trailing field of a text index.
(function() {
    "use strict";

    const coll = db.fts_trailing_fields;

    coll.drop();
    assert.commandWorked(coll.createIndex({a: 1, b: "text", c: 1}));
    assert.writeOK(coll.insert({a: 2, b: "lorem ipsum"}));

    assert.eq(0, coll.find({a: 2, $text: {$search: "lorem"}, c: {$exists: true}}).itcount());
    assert.eq(1, coll.find({a: 2, $text: {$search: "lorem"}, c: null}).itcount());
    assert.eq(1, coll.find({a: 2, $text: {$search: "lorem"}, c: {$exists: false}}).itcount());

    // An equality predicate on the leading field isn't useful, but it shouldn't cause any problems.
    // Same with an $elemMatch predicate on one of the trailing fields.
    coll.drop();
    assert.commandWorked(coll.createIndex({a: 1, b: "text", "c.d": 1}));
    assert.writeOK(coll.insert({a: 2, b: "lorem ipsum", c: {d: 3}}));
    assert.eq(0, coll.find({a: [1, 2], $text: {$search: "lorem"}}).itcount());
    assert.eq(0, coll.find({a: 2, $text: {$search: "lorem"}, c: {$elemMatch: {d: 3}}}).itcount());
}());
