// End-to-end testing for index scan explosion + merge sort.
// SERVER-5063 and SERVER-1205.
t = db.jstests_sortk;
t.drop();

function resetCollection() {
    t.drop();
    t.save({a: 1, b: 1});
    t.save({a: 1, b: 2});
    t.save({a: 1, b: 3});
    t.save({a: 2, b: 4});
    t.save({a: 2, b: 5});
    t.save({a: 2, b: 0});
}

resetCollection();
t.ensureIndex({a: 1, b: 1});

function simpleQuery(extraFields, sort, hint) {
    query = {a: {$in: [1, 2]}};
    Object.extend(query, extraFields);
    sort = sort || {b: 1};
    hint = hint || {a: 1, b: 1};
    return t.find(query).sort(sort).hint(hint);
}

function simpleQueryWithLimit(limit) {
    return simpleQuery().limit(limit);
}

// The limit is -1.
assert.eq(0, simpleQueryWithLimit(-1)[0].b);

// The limit is -2.
assert.eq(0, simpleQueryWithLimit(-2)[0].b);
assert.eq(1, simpleQueryWithLimit(-2)[1].b);

// A skip is applied.
assert.eq(1, simpleQueryWithLimit(-1).skip(1)[0].b);

// No limit is applied.
assert.eq(6, simpleQueryWithLimit(0).itcount());
assert.eq(6, simpleQueryWithLimit(0).explain(true).executionStats.totalKeysExamined);
assert.eq(5, simpleQueryWithLimit(0).skip(1).itcount());

// The query has additional constriants, preventing limit optimization.
assert.eq(2, simpleQuery({$where: 'this.b>=2'}).limit(-1)[0].b);

// The sort order is the reverse of the index order.
assert.eq(5, simpleQuery({}, {b: -1}).limit(-1)[0].b);

// The sort order is the reverse of the index order on a constrained field.
assert.eq(0, simpleQuery({}, {a: -1, b: 1}).limit(-1)[0].b);

// Without a hint, multiple cursors are attempted.
assert.eq(0, t.find({a: {$in: [1, 2]}}).sort({b: 1}).limit(-1)[0].b);
explain = t.find({a: {$in: [1, 2]}}).sort({b: 1}).limit(-1).explain(true);
assert.eq(1, explain.executionStats.nReturned);

// The expected first result now comes from the first interval.
t.remove({b: 0});
assert.eq(1, simpleQueryWithLimit(-1)[0].b);

// With three intervals.

function inThreeIntervalQueryWithLimit(limit) {
    return t.find({a: {$in: [1, 2, 3]}}).sort({b: 1}).hint({a: 1, b: 1}).limit(limit);
}

assert.eq(1, inThreeIntervalQueryWithLimit(-1)[0].b);
assert.eq(1, inThreeIntervalQueryWithLimit(-2)[0].b);
assert.eq(2, inThreeIntervalQueryWithLimit(-2)[1].b);
t.save({a: 3, b: 0});
assert.eq(0, inThreeIntervalQueryWithLimit(-1)[0].b);
assert.eq(0, inThreeIntervalQueryWithLimit(-2)[0].b);
assert.eq(1, inThreeIntervalQueryWithLimit(-2)[1].b);

// The index is multikey.
t.remove({});
t.save({a: 1, b: [0, 1, 2]});
t.save({a: 2, b: [0, 1, 2]});
t.save({a: 1, b: 5});
assert.eq(3, simpleQueryWithLimit(-3).itcount());

// The index ordering is reversed.
resetCollection();
t.ensureIndex({a: 1, b: -1});

// The sort order is consistent with the index order.
assert.eq(5, simpleQuery({}, {b: -1}, {a: 1, b: -1}).limit(-1)[0].b);

// The sort order is the reverse of the index order.
assert.eq(0, simpleQuery({}, {b: 1}, {a: 1, b: -1}).limit(-1)[0].b);

// An equality constraint precedes the $in constraint.
t.drop();
t.ensureIndex({a: 1, b: 1, c: 1});
t.save({a: 0, b: 0, c: -1});
t.save({a: 0, b: 2, c: 1});
t.save({a: 1, b: 1, c: 1});
t.save({a: 1, b: 1, c: 2});
t.save({a: 1, b: 1, c: 3});
t.save({a: 1, b: 2, c: 4});
t.save({a: 1, b: 2, c: 5});
t.save({a: 1, b: 2, c: 0});

function eqInQueryWithLimit(limit) {
    return t.find({a: 1, b: {$in: [1, 2]}}).sort({c: 1}).hint({a: 1, b: 1, c: 1}).limit(limit);
}

function andEqInQueryWithLimit(limit) {
    return t.find({$and: [{a: 1}, {b: {$in: [1, 2]}}]})
        .sort({c: 1})
        .hint({a: 1, b: 1, c: 1})
        .limit(limit);
}

// The limit is -1.
assert.eq(0, eqInQueryWithLimit(-1)[0].c);
assert.eq(0, andEqInQueryWithLimit(-1)[0].c);

// The limit is -2.
assert.eq(0, eqInQueryWithLimit(-2)[0].c);
assert.eq(1, eqInQueryWithLimit(-2)[1].c);
assert.eq(0, andEqInQueryWithLimit(-2)[0].c);
assert.eq(1, andEqInQueryWithLimit(-2)[1].c);

function inQueryWithLimit(limit, sort) {
    sort = sort || {b: 1};
    return t.find({a: {$in: [0, 1]}}).sort(sort).hint({a: 1, b: 1, c: 1}).limit(limit);
}

// The index has two suffix fields unconstrained by the query.
assert.eq(0, inQueryWithLimit(-1)[0].b);

// The index has two ordered suffix fields unconstrained by the query.
assert.eq(0, inQueryWithLimit(-1, {b: 1, c: 1})[0].b);

// The index has two ordered suffix fields unconstrained by the query and the limit is -2.
assert.eq(0, inQueryWithLimit(-2, {b: 1, c: 1})[0].b);
assert.eq(1, inQueryWithLimit(-2, {b: 1, c: 1})[1].b);
