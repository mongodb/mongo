// Test sorting with skipping and multiple candidate query plans.
(function() {
    "use strict";

    const coll = db.jstests_sortc;
    coll.drop();

    assert.writeOK(coll.insert({a: 1}));
    assert.writeOK(coll.insert({a: 2}));

    function checkA(a, sort, skip, query) {
        query = query || {};
        assert.eq(a, coll.find(query).sort(sort).skip(skip)[0].a);
    }

    function checkSortAndSkip() {
        checkA(1, {a: 1}, 0);
        checkA(2, {a: 1}, 1);

        checkA(1, {a: 1}, 0, {a: {$gt: 0}, b: null});
        checkA(2, {a: 1}, 1, {a: {$gt: 0}, b: null});

        checkA(2, {a: -1}, 0);
        checkA(1, {a: -1}, 1);

        checkA(2, {a: -1}, 0, {a: {$gt: 0}, b: null});
        checkA(1, {a: -1}, 1, {a: {$gt: 0}, b: null});
    }

    checkSortAndSkip();

    assert.commandWorked(coll.createIndex({a: 1}));
    checkSortAndSkip();
}());
