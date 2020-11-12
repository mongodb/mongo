// Test bounds with multiple inequalities and sorting.

function checkResults(expected, cursor) {
    assert.eq(expected.length, cursor.count());
    for (i = 0; i < expected.length; ++i) {
        assert.eq(expected[i].a, cursor[i].a);
        assert.eq(expected[i].b, cursor[i].b);
    }
}

function testMultipleInequalities(db) {
    r = db.ed_db_cursor_mi;
    r.drop();

    z = [{a: 1, b: 2}, {a: 3, b: 4}, {a: 5, b: 6}, {a: 7, b: 8}];
    for (i = 0; i < z.length; ++i)
        r.save(z[i]);
    idx = {a: 1, b: 1};
    rIdx = {a: -1, b: -1};
    r.ensureIndex(idx);

    checkResults([z[2], z[3]], r.find({a: {$gt: 3}}).sort(idx).hint(idx));
    checkResults([z[2]], r.find({a: {$gt: 3, $lt: 7}}).sort(idx).hint(idx));
    checkResults([z[2]], r.find({a: {$gt: 3, $lt: 7, $lte: 5}}).sort(idx).hint(idx));

    checkResults([z[3], z[2]], r.find({a: {$gt: 3}}).sort(rIdx).hint(idx));
    checkResults([z[2]], r.find({a: {$gt: 3, $lt: 7}}).sort(rIdx).hint(idx));
    checkResults([z[2]], r.find({a: {$gt: 3, $lt: 7, $lte: 5}}).sort(rIdx).hint(idx));

    checkResults(
        [z[1], z[2]],
        r.find({a: {$gt: 1, $lt: 7, $gte: 3, $lte: 5}, b: {$gt: 2, $lt: 8, $gte: 4, $lte: 6}})
            .sort(idx)
            .hint(idx));
    checkResults(
        [z[2], z[1]],
        r.find({a: {$gt: 1, $lt: 7, $gte: 3, $lte: 5}, b: {$gt: 2, $lt: 8, $gte: 4, $lte: 6}})
            .sort(rIdx)
            .hint(idx));

    checkResults(
        [z[1], z[2]],
        r.find({a: {$gte: 1, $lte: 7, $gt: 2, $lt: 6}, b: {$gte: 2, $lte: 8, $gt: 3, $lt: 7}})
            .sort(idx)
            .hint(idx));
    checkResults(
        [z[2], z[1]],
        r.find({a: {$gte: 1, $lte: 7, $gt: 2, $lt: 6}, b: {$gte: 2, $lte: 8, $gt: 3, $lt: 7}})
            .sort(rIdx)
            .hint(idx));
}

testMultipleInequalities(db);
