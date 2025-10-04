// @tags: [
//   requires_getmore,
// ]

let t = db.jstests_or1;
t.drop();

let checkArrs = function (a, b) {
    assert.eq(a.length, b.length);
    let aStr = [];
    let bStr = [];
    a.forEach(function (x) {
        aStr.push(tojson(x));
    });
    b.forEach(function (x) {
        bStr.push(tojson(x));
    });
    for (let i = 0; i < aStr.length; ++i) {
        assert.neq(-1, bStr.indexOf(aStr[i]));
    }
};

let doTest = function () {
    t.save({_id: 0, a: 1});
    t.save({_id: 1, a: 2});
    t.save({_id: 2, b: 1});
    t.save({_id: 3, b: 2});
    t.save({_id: 4, a: 1, b: 1});
    t.save({_id: 5, a: 1, b: 2});
    t.save({_id: 6, a: 2, b: 1});
    t.save({_id: 7, a: 2, b: 2});

    assert.throws(function () {
        t.find({$or: "a"}).toArray();
    });
    assert.throws(function () {
        t.find({$or: []}).toArray();
    });
    assert.throws(function () {
        t.find({$or: ["a"]}).toArray();
    });

    let a1 = t.find({$or: [{a: 1}]}).toArray();
    checkArrs(
        [
            {_id: 0, a: 1},
            {_id: 4, a: 1, b: 1},
            {_id: 5, a: 1, b: 2},
        ],
        a1,
    );

    let a1b2 = t.find({$or: [{a: 1}, {b: 2}]}).toArray();
    checkArrs(
        [
            {_id: 0, a: 1},
            {_id: 3, b: 2},
            {_id: 4, a: 1, b: 1},
            {_id: 5, a: 1, b: 2},
            {_id: 7, a: 2, b: 2},
        ],
        a1b2,
    );

    t.drop();
    t.save({a: [0, 1], b: [0, 1]});
    assert.eq(1, t.find({$or: [{a: {$in: [0, 1]}}]}).toArray().length);
    assert.eq(1, t.find({$or: [{b: {$in: [0, 1]}}]}).toArray().length);
    assert.eq(1, t.find({$or: [{a: {$in: [0, 1]}}, {b: {$in: [0, 1]}}]}).toArray().length);
};

doTest();

// not part of SERVER-1003, but good check for subseq. implementations
t.drop();
t.createIndex({a: 1});
doTest();

t.drop();
t.createIndex({b: 1});
doTest();

t.drop();
t.createIndex({a: 1, b: 1});
doTest();
