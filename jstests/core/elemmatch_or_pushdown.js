/**
 * Tests that an $elemMatch-$or query is evaluated correctly. Designed to reproduce SERVER-33005.
 */
(function() {
    "use strict";

    const coll = db.elemmatch_or_pushdown;
    coll.drop();

    assert.writeOK(coll.insert({_id: 0, a: 1, b: [{c: 4}]}));
    assert.writeOK(coll.insert({_id: 1, a: 2, b: [{c: 4}]}));
    assert.writeOK(coll.insert({_id: 2, a: 2, b: [{c: 5}]}));
    assert.writeOK(coll.insert({_id: 3, a: 1, b: [{c: 5}]}));
    assert.writeOK(coll.insert({_id: 4, a: 1, b: [{c: 6}]}));
    assert.writeOK(coll.insert({_id: 5, a: 1, b: [{c: 7}]}));
    assert.commandWorked(coll.createIndex({a: 1, "b.c": 1}));

    assert.eq(coll.find({a: 1, b: {$elemMatch: {$or: [{c: 4}, {c: 5}]}}}).sort({_id: 1}).toArray(),
              [{_id: 0, a: 1, b: [{c: 4}]}, {_id: 3, a: 1, b: [{c: 5}]}]);
    assert.eq(coll.find({a: 1, $or: [{a: 2}, {b: {$elemMatch: {$or: [{c: 4}, {c: 5}]}}}]})
                  .sort({_id: 1})
                  .toArray(),
              [{_id: 0, a: 1, b: [{c: 4}]}, {_id: 3, a: 1, b: [{c: 5}]}]);

    coll.drop();
    assert.writeOK(coll.insert({_id: 0, a: 5, b: [{c: [{f: 8}], d: 6}]}));
    assert.writeOK(coll.insert({_id: 1, a: 4, b: [{c: [{f: 8}], d: 6}]}));
    assert.writeOK(coll.insert({_id: 2, a: 5, b: [{c: [{f: 8}], d: 7}]}));
    assert.writeOK(coll.insert({_id: 3, a: 4, b: [{c: [{f: 9}], d: 6}]}));
    assert.writeOK(coll.insert({_id: 4, a: 5, b: [{c: [{f: 8}], e: 7}]}));
    assert.writeOK(coll.insert({_id: 5, a: 4, b: [{c: [{f: 8}], e: 7}]}));
    assert.writeOK(coll.insert({_id: 6, a: 5, b: [{c: [{f: 8}], e: 8}]}));
    assert.writeOK(coll.insert({_id: 7, a: 5, b: [{c: [{f: 9}], e: 7}]}));
    assert.commandWorked(coll.createIndex({"b.d": 1, "b.c.f": 1}));
    assert.commandWorked(coll.createIndex({"b.e": 1, "b.c.f": 1}));

    assert.eq(coll.find({a: 5, b: {$elemMatch: {c: {$elemMatch: {f: 8}}, $or: [{d: 6}, {e: 7}]}}})
                  .sort({_id: 1})
                  .toArray(),
              [{_id: 0, a: 5, b: [{c: [{f: 8}], d: 6}]}, {_id: 4, a: 5, b: [{c: [{f: 8}], e: 7}]}]);
}());
