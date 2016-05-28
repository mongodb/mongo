t = db.jstests_nin;
t.drop();

function checkEqual(name, key, value) {
    var o = {};
    o[key] = {$in: [value]};
    var i = t.find(o).count();
    o[key] = {$nin: [value]};
    var n = t.find(o).count();

    assert.eq(t.find().count(),
              i + n,
              "checkEqual " + name + " $in + $nin != total | " + i + " + " + n + " != " +
                  t.find().count());
}

doTest = function(n) {

    t.save({a: [1, 2, 3]});
    t.save({a: [1, 2, 4]});
    t.save({a: [1, 8, 5]});
    t.save({a: [1, 8, 6]});
    t.save({a: [1, 9, 7]});

    assert.eq(5, t.find({a: {$nin: [10]}}).count(), n + " A");
    assert.eq(0, t.find({a: {$ne: 1}}).count(), n + " B");
    assert.eq(0, t.find({a: {$nin: [1]}}).count(), n + " C");
    assert.eq(0, t.find({a: {$nin: [1, 2]}}).count(), n + " D");
    assert.eq(3, t.find({a: {$nin: [2]}}).count(), n + " E");
    assert.eq(3, t.find({a: {$nin: [8]}}).count(), n + " F");
    assert.eq(4, t.find({a: {$nin: [9]}}).count(), n + " G");
    assert.eq(4, t.find({a: {$nin: [3]}}).count(), n + " H");
    assert.eq(3, t.find({a: {$nin: [2, 3]}}).count(), n + " I");
    assert.eq(1, t.find({a: {$ne: 8, $nin: [2, 3]}}).count(), n + " I2");

    checkEqual(n + " A", "a", 5);

    t.save({a: [2, 2]});
    assert.eq(3, t.find({a: {$nin: [2, 2]}}).count(), n + " J");

    t.save({a: [[2]]});
    assert.eq(4, t.find({a: {$nin: [2]}}).count(), n + " K");

    t.save({a: [{b: [10, 11]}, 11]});
    checkEqual(n + " B", "a", 5);
    checkEqual(n + " C", "a.b", 5);

    assert.eq(7, t.find({'a.b': {$nin: [10]}}).count(), n + " L");
    assert.eq(7, t.find({'a.b': {$nin: [[10, 11]]}}).count(), n + " M");
    assert.eq(7, t.find({a: {$nin: [11]}}).count(), n + " N");

    t.save({a: {b: [20, 30]}});
    assert.eq(1, t.find({'a.b': {$all: [20]}}).count(), n + " O");
    assert.eq(1, t.find({'a.b': {$all: [20, 30]}}).count(), n + " P");
};

doTest("no index");
t.drop();
t.ensureIndex({a: 1});
doTest("with index");
