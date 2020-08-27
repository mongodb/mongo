// Test inequality bounds with multi-field sorting
//
// This test is currently disabled in SBE passthroughs due to bugs that cause problems on big-endian
// platforms. Once these bugs are fixed, this test should be re-enabled for SBE.
// @tags: [
//   sbe_incompatible,
// ]

function checkResults(expected, cursor) {
    assert.eq(expected.length, cursor.count());
    for (i = 0; i < expected.length; ++i) {
        assert.eq(expected[i].a, cursor[i].a);
        assert.eq(expected[i].b, cursor[i].b);
    }
}

function testConstrainedFindMultiFieldSorting(db) {
    r = db.ed_db_cursor4_cfmfs;
    r.drop();

    entries = [{a: 0, b: 0}, {a: 0, b: 1}, {a: 1, b: 1}, {a: 1, b: 1}, {a: 2, b: 0}];
    for (i = 0; i < entries.length; ++i)
        r.save(entries[i]);
    r.ensureIndex({a: 1, b: 1});
    reverseEntries = entries.slice();
    reverseEntries.reverse();

    checkResults(entries.slice(2, 4), r.find({a: 1, b: 1}).sort({a: 1, b: 1}).hint({a: 1, b: 1}));
    checkResults(entries.slice(2, 4), r.find({a: 1, b: 1}).sort({a: -1, b: -1}).hint({a: 1, b: 1}));

    checkResults(entries.slice(2, 5), r.find({a: {$gt: 0}}).sort({a: 1, b: 1}).hint({a: 1, b: 1}));
    checkResults(reverseEntries.slice(0, 3),
                 r.find({a: {$gt: 0}}).sort({a: -1, b: -1}).hint({a: 1, b: 1}));
    checkResults(entries.slice(0, 4), r.find({a: {$lt: 2}}).sort({a: 1, b: 1}).hint({a: 1, b: 1}));
    checkResults(reverseEntries.slice(1, 5),
                 r.find({a: {$lt: 2}}).sort({a: -1, b: -1}).hint({a: 1, b: 1}));

    checkResults(entries.slice(4, 5),
                 r.find({a: {$gt: 0}, b: {$lt: 1}}).sort({a: 1, b: 1}).hint({a: 1, b: 1}));
    checkResults(entries.slice(2, 4),
                 r.find({a: {$gt: 0}, b: {$gt: 0}}).sort({a: 1, b: 1}).hint({a: 1, b: 1}));

    checkResults(reverseEntries.slice(0, 1),
                 r.find({a: {$gt: 0}, b: {$lt: 1}}).sort({a: -1, b: -1}).hint({a: 1, b: 1}));
    checkResults(reverseEntries.slice(1, 3),
                 r.find({a: {$gt: 0}, b: {$gt: 0}}).sort({a: -1, b: -1}).hint({a: 1, b: 1}));

    checkResults(entries.slice(0, 1),
                 r.find({a: {$lt: 2}, b: {$lt: 1}}).sort({a: 1, b: 1}).hint({a: 1, b: 1}));
    checkResults(entries.slice(1, 4),
                 r.find({a: {$lt: 2}, b: {$gt: 0}}).sort({a: 1, b: 1}).hint({a: 1, b: 1}));

    checkResults(reverseEntries.slice(4, 5),
                 r.find({a: {$lt: 2}, b: {$lt: 1}}).sort({a: -1, b: -1}).hint({a: 1, b: 1}));
    checkResults(reverseEntries.slice(1, 4),
                 r.find({a: {$lt: 2}, b: {$gt: 0}}).sort({a: -1, b: -1}).hint({a: 1, b: 1}));
}

testConstrainedFindMultiFieldSorting(db);
