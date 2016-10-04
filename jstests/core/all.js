t = db.jstests_all;
t.drop();

doTest = function() {

    t.save({a: [1, 2, 3]});
    t.save({a: [1, 2, 4]});
    t.save({a: [1, 8, 5]});
    t.save({a: [1, 8, 6]});
    t.save({a: [1, 9, 7]});
    t.save({a: []});
    t.save({});

    assert.eq(5, t.find({a: {$all: [1]}}).count());
    assert.eq(2, t.find({a: {$all: [1, 2]}}).count());
    assert.eq(2, t.find({a: {$all: [1, 8]}}).count());
    assert.eq(1, t.find({a: {$all: [1, 3]}}).count());
    assert.eq(2, t.find({a: {$all: [2]}}).count());
    assert.eq(1, t.find({a: {$all: [2, 3]}}).count());
    assert.eq(2, t.find({a: {$all: [2, 1]}}).count());

    t.save({a: [2, 2]});
    assert.eq(3, t.find({a: {$all: [2, 2]}}).count());

    t.save({a: [[2]]});
    assert.eq(3, t.find({a: {$all: [2]}}).count());

    t.save({a: [{b: [10, 11]}, 11]});
    assert.eq(1, t.find({'a.b': {$all: [10]}}).count());
    assert.eq(1, t.find({a: {$all: [11]}}).count());

    t.save({a: {b: [20, 30]}});
    assert.eq(1, t.find({'a.b': {$all: [20]}}).count());
    assert.eq(1, t.find({'a.b': {$all: [20, 30]}}).count());

    assert.eq(5, t.find({a: {$all: [1]}}).count(), "E1");
    assert.eq(0, t.find({a: {$all: [19]}}).count(), "E2");
    assert.eq(0, t.find({a: {$all: []}}).count(), "E3");

};

doTest();
t.drop();
t.ensureIndex({a: 1});
doTest();
