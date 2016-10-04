// Check nonoverlapping $in/$all with multikeys SERVER-2165

t = db.jstests_indexl;

function test(t) {
    t.save({a: [1, 2]});
    assert.eq(1, t.count({a: {$all: [1], $in: [2]}}));
    assert.eq(1, t.count({a: {$all: [2], $in: [1]}}));
    assert.eq(1, t.count({a: {$in: [2], $all: [1]}}));
    assert.eq(1, t.count({a: {$in: [1], $all: [2]}}));
    assert.eq(1, t.count({a: {$all: [1], $in: [2]}}));
    t.save({a: [3, 4]});
    t.save({a: [2, 3]});
    t.save({a: [1, 2, 3, 4]});
    assert.eq(2, t.count({a: {$in: [2], $all: [1]}}));
    assert.eq(1, t.count({a: {$in: [3], $all: [1, 2]}}));
    assert.eq(1, t.count({a: {$in: [1], $all: [3]}}));
    assert.eq(2, t.count({a: {$in: [2, 3], $all: [1]}}));
    assert.eq(1, t.count({a: {$in: [4], $all: [2, 3]}}));
    assert.eq(3, t.count({a: {$in: [1, 3], $all: [2]}}));
}

t.drop();
test(t);
t.drop();
t.ensureIndex({a: 1});
test(t);