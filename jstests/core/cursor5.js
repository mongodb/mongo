// Test bounds with subobject indexes.

function checkResults(expected, cursor) {
    assert.eq(expected.length, cursor.count());
    for (i = 0; i < expected.length; ++i) {
        assert.eq(expected[i].a.b, cursor[i].a.b);
        assert.eq(expected[i].a.c, cursor[i].a.c);
        assert.eq(expected[i].a.d, cursor[i].a.d);
        assert.eq(expected[i].e, cursor[i].e);
    }
}

function testBoundsWithSubobjectIndexes(db) {
    r = db.ed_db_cursor5_bwsi;
    r.drop();

    z = [
        {a: {b: 1, c: 2, d: 3}, e: 4},
        {a: {b: 1, c: 2, d: 3}, e: 5},
        {a: {b: 1, c: 2, d: 4}, e: 4},
        {a: {b: 1, c: 2, d: 4}, e: 5},
        {a: {b: 2, c: 2, d: 3}, e: 4},
        {a: {b: 2, c: 2, d: 3}, e: 5}
    ];
    for (i = 0; i < z.length; ++i)
        r.save(z[i]);
    idx = {"a.d": 1, a: 1, e: -1};
    rIdx = {"a.d": -1, a: -1, e: 1};
    r.ensureIndex(idx);

    checkResults([z[0], z[4], z[2]], r.find({e: 4}).sort(idx).hint(idx));
    checkResults([z[1], z[3]], r.find({e: {$gt: 4}, "a.b": 1}).sort(idx).hint(idx));

    checkResults([z[2], z[4], z[0]], r.find({e: 4}).sort(rIdx).hint(idx));
    checkResults([z[3], z[1]], r.find({e: {$gt: 4}, "a.b": 1}).sort(rIdx).hint(idx));
}

testBoundsWithSubobjectIndexes(db);
