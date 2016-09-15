/**
 * This test ensures that queries using simple ranges handle bound inclusion properly.
 */
(function() {
    'use strict';
    var coll = db.query_bound_inclusion;
    coll.drop();
    assert.writeOK(coll.insert({a: 1, b: 1}));
    assert.writeOK(coll.insert({a: 2, b: 2}));
    assert.writeOK(coll.insert({a: 3, b: 3}));

    assert.commandWorked(coll.createIndex({a: 1}));

    var res = coll.find().sort({a: 1}).toArray();
    assert.eq(res.length, 3);
    assert.eq(res[0].a, 1);
    assert.eq(res[1].a, 2);
    assert.eq(res[2].a, 3);

    res = coll.find().sort({a: -1}).toArray();
    assert.eq(res.length, 3);
    assert.eq(res[0].a, 3);
    assert.eq(res[1].a, 2);
    assert.eq(res[2].a, 1);

    res = coll.find().min({a: 1}).max({a: 3}).toArray();
    assert.eq(res.length, 2);
    assert.eq(res[0].a, 1);
    assert.eq(res[1].a, 2);

    res = coll.find().min({a: 1}).max({a: 3}).sort({a: -1}).toArray();
    assert.eq(res.length, 2);
    assert.eq(res[0].a, 2);
    assert.eq(res[1].a, 1);

    assert.commandWorked(coll.createIndex({b: -1}));

    res = coll.find().sort({b: -1}).toArray();
    assert.eq(res.length, 3);
    assert.eq(res[0].b, 3);
    assert.eq(res[1].b, 2);
    assert.eq(res[2].b, 1);

    res = coll.find().sort({b: 1}).toArray();
    assert.eq(res.length, 3);
    assert.eq(res[0].b, 1);
    assert.eq(res[1].b, 2);
    assert.eq(res[2].b, 3);

    res = coll.find().min({b: 3}).max({b: 1}).toArray();
    assert.eq(res.length, 2);
    assert.eq(res[0].b, 3);
    assert.eq(res[1].b, 2);

    res = coll.find().min({b: 3}).max({b: 1}).sort({b: 1}).toArray();
    assert.eq(res.length, 2);
    assert.eq(res[0].b, 2);
    assert.eq(res[1].b, 3);
})();