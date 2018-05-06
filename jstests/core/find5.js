// @tags: [requires_fastcount]

(function() {
    "use strict";

    const coll = db.find5;
    coll.drop();

    assert.writeOK(coll.insert({a: 1}));
    assert.writeOK(coll.insert({b: 5}));

    assert.eq(2, coll.find({}, {b: 1}).count(), "A");

    function getIds(projection) {
        return coll.find({}, projection).map(doc => doc._id);
    }

    assert.eq(Array.tojson(getIds(null)), Array.tojson(getIds({})), "B1 ");
    assert.eq(Array.tojson(getIds(null)), Array.tojson(getIds({a: 1})), "B2 ");
    assert.eq(Array.tojson(getIds(null)), Array.tojson(getIds({b: 1})), "B3 ");
    assert.eq(Array.tojson(getIds(null)), Array.tojson(getIds({c: 1})), "B4 ");

    let results = coll.find({}, {a: 1}).sort({a: -1});
    let first = results[0];
    assert.eq(1, first.a, "C1");
    assert.isnull(first.b, "C2");

    let second = results[1];
    assert.isnull(second.a, "C3");
    assert.isnull(second.b, "C4");

    results = coll.find({}, {b: 1}).sort({a: -1});
    first = results[0];
    assert.isnull(first.a, "C5");
    assert.isnull(first.b, "C6");

    second = results[1];
    assert.isnull(second.a, "C7");
    assert.eq(5, second.b, "C8");

    assert(coll.drop());

    assert.writeOK(coll.insert({a: 1, b: {c: 2, d: 3, e: 4}}));
    assert.eq(2, coll.findOne({}, {"b.c": 1}).b.c, "D");

    const o = coll.findOne({}, {"b.c": 1, "b.d": 1});
    assert(o.b.c, "E 1");
    assert(o.b.d, "E 2");
    assert(!o.b.e, "E 3");

    assert(!coll.findOne({}, {"b.c": 1}).b.d, "F");

    assert(coll.drop());
    assert.writeOK(coll.insert({a: {b: {c: 1}}}));
    assert.eq(1, coll.findOne({}, {"a.b.c": 1}).a.b.c, "G");
}());
