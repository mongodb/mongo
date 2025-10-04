// SERVER-1089 Test and/or/nor nesting
// @tags: [
//   requires_getmore
// ]

let t = db.jstests_andor;
t.drop();

// not ok
function ok(q) {
    assert.eq(1, t.find(q).itcount());
}

t.save({a: 1});

let test = function () {
    ok({a: 1});

    ok({$and: [{a: 1}]});
    ok({$or: [{a: 1}]});

    ok({$and: [{$and: [{a: 1}]}]});
    ok({$or: [{$or: [{a: 1}]}]});

    ok({$and: [{$or: [{a: 1}]}]});
    ok({$or: [{$and: [{a: 1}]}]});

    ok({$and: [{$and: [{$or: [{a: 1}]}]}]});
    ok({$and: [{$or: [{$and: [{a: 1}]}]}]});
    ok({$or: [{$and: [{$and: [{a: 1}]}]}]});

    ok({$or: [{$and: [{$or: [{a: 1}]}]}]});

    // now test $nor

    ok({$and: [{a: 1}]});
    ok({$nor: [{a: 2}]});

    ok({$and: [{$and: [{a: 1}]}]});
    ok({$nor: [{$nor: [{a: 1}]}]});

    ok({$and: [{$nor: [{a: 2}]}]});
    ok({$nor: [{$and: [{a: 2}]}]});

    ok({$and: [{$and: [{$nor: [{a: 2}]}]}]});
    ok({$and: [{$nor: [{$and: [{a: 2}]}]}]});
    ok({$nor: [{$and: [{$and: [{a: 2}]}]}]});

    ok({$nor: [{$and: [{$nor: [{a: 1}]}]}]});
};

test();
t.createIndex({a: 1});
test();

// Test an inequality base match.

test = function () {
    ok({a: {$ne: 2}});

    ok({$and: [{a: {$ne: 2}}]});
    ok({$or: [{a: {$ne: 2}}]});

    ok({$and: [{$and: [{a: {$ne: 2}}]}]});
    ok({$or: [{$or: [{a: {$ne: 2}}]}]});

    ok({$and: [{$or: [{a: {$ne: 2}}]}]});
    ok({$or: [{$and: [{a: {$ne: 2}}]}]});

    ok({$and: [{$and: [{$or: [{a: {$ne: 2}}]}]}]});
    ok({$and: [{$or: [{$and: [{a: {$ne: 2}}]}]}]});
    ok({$or: [{$and: [{$and: [{a: {$ne: 2}}]}]}]});

    ok({$or: [{$and: [{$or: [{a: {$ne: 2}}]}]}]});

    // now test $nor

    ok({$and: [{a: {$ne: 2}}]});
    ok({$nor: [{a: {$ne: 1}}]});

    ok({$and: [{$and: [{a: {$ne: 2}}]}]});
    ok({$nor: [{$nor: [{a: {$ne: 2}}]}]});

    ok({$and: [{$nor: [{a: {$ne: 1}}]}]});
    ok({$nor: [{$and: [{a: {$ne: 1}}]}]});

    ok({$and: [{$and: [{$nor: [{a: {$ne: 1}}]}]}]});
    ok({$and: [{$nor: [{$and: [{a: {$ne: 1}}]}]}]});
    ok({$nor: [{$and: [{$and: [{a: {$ne: 1}}]}]}]});

    ok({$nor: [{$and: [{$nor: [{a: {$ne: 2}}]}]}]});
};

t.drop();
t.save({a: 1});
test();
t.createIndex({a: 1});
test();
